#pragma once
#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdint>

namespace B3 {

// Vintage 1958 Hammond B3 91-Tonewheel Generator:
// - Genuine 60 Hz synchronous shaft gear ratios locked to A4 = 440.0 Hz
// - 4 progressive electromagnetic tonewheel waveform banks (scalloped sub-wheels,
//   warm iron-rich low-mids, subtle coil-asymmetry upper-mids, LC-filtered highs)
//   + deterministic per-pickup calibration micro-variance
// - Real 9-busbar palladium wire contact stagger (velocity-sensitive 1.0..3.5 ms
//   sequential busbar closure per key, producing organic harmonicchiff/spit)
// - Dual-band woody + palladium contact key-click transient
// - Generator compartment crosstalk (4-octave companion wheels +/-48, octave/fifth
//   sympathetic pickup bleed, and adjacent shaft neighbors)
// - Separate post-scanner Harmonic Percussion bus (bypasses V/C scanner like a
//   real stock B3 so 2nd/3rd percussion stays tight, woody, and punchy!)
class TonewheelEngine {
public:
  static constexpr int kNumWheels = 91;
  static constexpr int kNumDrawbars = 9;
  static constexpr int kDrawbarSemitones[kNumDrawbars] =
      {-12, 7, 0, 12, 19, 24, 28, 31, 36};

  void prepare(double sampleRate) {
    sr = sampleRate > 8000.0 ? sampleRate : 44100.0;
    const float srF = (float)sr;

    static constexpr double kGearHzOct4[12] = {
        320.0 * (85.0 / 104.0), // C4  261.5385 Hz
        320.0 * (71.0 / 82.0),  // C#4 277.0732 Hz
        320.0 * (67.0 / 73.0),  // D4  293.6986 Hz
        320.0 * (35.0 / 36.0),  // D#4 311.1111 Hz
        320.0 * (69.0 / 67.0),  // E4  329.5522 Hz
        320.0 * (12.0 / 11.0),  // F4  349.0909 Hz
        320.0 * (37.0 / 32.0),  // F#4 370.0000 Hz
        320.0 * (49.0 / 40.0),  // G4  392.0000 Hz
        320.0 * (48.0 / 37.0),  // G#4 415.1351 Hz
        320.0 * (11.0 / 8.0),   // A4  440.0000 Hz
        320.0 * (67.0 / 46.0),  // A#4 466.0870 Hz
        320.0 * (54.0 / 35.0)   // B4  493.7143 Hz
    };

    rngState = 0x58B31958u;
    for (int i = 0; i < kNumWheels; ++i) {
      const int octShift = (i / 12) - 3; // wheel 0 = C1
      wheelFreq[(size_t)i] =
          (float)(kGearHzOct4[i % 12] * std::ldexp(1.0, octShift));
      wheelPhaseInc[(size_t)i] = wheelFreq[(size_t)i] / srF;

      // Vintage pickup rod distance & matching-transformer curve:
      // Rich chest in low octaves, vocal bite around wheels 44..68 (1.0..3.2 kHz),
      // natural transformer HF taper at the very top.
      float baseGain = 1.0f;
      if (i < 12) {
        baseGain = 1.08f;
      } else if (i <= 40) {
        baseGain = 1.02f + 0.08f * ((float)(i - 12) / 28.0f);
      } else if (i <= 68) {
        baseGain = 1.10f + 0.08f * std::sin(3.14159265f * (float)(i - 40) / 28.0f);
      } else {
        const float t = (float)(i - 68) / (float)(kNumWheels - 1 - 68);
        baseGain = 1.10f - 0.36f * t;
      }
      // Subtle +/- 4.5% vintage pickup-rod calibration variance
      const float rodTrim = 0.955f + 0.090f * xorShift();
      wheelPickupGain[(size_t)i] = baseGain * rodTrim;
    }

    // Crisp palladium contact closure (~0.55 ms) & smooth release (~2.1 ms)
    wheelAtk = 1.0f - std::exp(-1.0f / (0.00055f * srF));
    wheelRel = 1.0f - std::exp(-1.0f / (0.00210f * srF));

    percDecayFast = std::exp(-1.0f / (0.030f * srF)); // Snappy ~190 ms B3 Fast
    percDecaySlow = std::exp(-1.0f / (0.086f * srF)); // Singing ~540 ms B3 Slow
    percDecayRel  = std::exp(-1.0f / (0.004f * srF));

    // Dual-band key click: Band 1 = 920 Hz woody manual thump, Band 2 = 2450 Hz contact spark
    clickWoodDecay  = std::exp(-1.0f / (0.0022f * srF));
    clickSparkDecay = std::exp(-1.0f / (0.0009f * srF));
    setupSvf(woodSvf,  juceClamp(920.0f,  250.0f, srF * 0.40f), 1.05f, srF);
    setupSvf(sparkSvf, juceClamp(2450.0f, 500.0f, srF * 0.42f), 1.25f, srF);

    buildLuts();

    rngState = 0x13572468u;
    for (int i = 0; i < kNumWheels; ++i)
      phase[(size_t)i] = xorShift();

    reset();
  }

  void reset() {
    wheelTargetAmp.fill(0.0f);
    wheelCurAmp.fill(0.0f);
    percWheelTargetAmp.fill(0.0f);
    percWheelCurAmp.fill(0.0f);
    for (auto &arr : contactDelaySamples)
      arr.fill(0);
    staggerActiveCount = 0;
    woodEnergy = sparkEnergy = 0.0f;
    woodSvf.ic1 = woodSvf.ic2 = 0.0f;
    sparkSvf.ic1 = sparkSvf.ic2 = 0.0f;
    percEnv = 0.0f;
    percActive = false;
    activeNoteCount = 0;
    held.reset();
    ampsDirty = true;
  }

  void noteOn(int note, float velocity = 0.85f) {
    note &= 127;
    if (held.test((size_t)note))
      return;
    held.set((size_t)note, true);
    ++activeNoteCount;

    // Velocity-sensitive 9-busbar mechanical contact stagger:
    // Fast slap = ~0.9 ms tight cluster; gentle touch = ~3.4 ms rolling busbar kiss
    const float v = juceClamp(velocity, 0.05f, 1.0f);
    if (clickDepth > 0.005f) {
      const float maxSpreadSec = (0.0034f - 0.0024f * v) * (0.45f + 0.55f * clickDepth);
      const int maxSamples = (int)(maxSpreadSec * (float)sr);
      auto &cd = contactDelaySamples[(size_t)note];
      for (int d = 0; d < kNumDrawbars; ++d) {
        // Deterministic per-note + per-busbar mechanical wire height profile
        const uint32_t h = (uint32_t)(note * 131 + d * 37 + 17);
        const float jitter = (float)((h * 2654435761u) & 0xFFFFu) / 65535.0f;
        const int dly = (d == 2) ? 0 : (int)((float)maxSamples * (0.15f + 0.85f * jitter));
        cd[(size_t)d] = dly;
        if (dly > 0)
          ++staggerActiveCount;
      }
      const float e = clickDepth * (0.62f + 0.38f * v);
      woodEnergy  = juceMax(woodEnergy,  juceClamp(e, 0.0f, 1.25f));
      sparkEnergy = juceMax(sparkEnergy, juceClamp(e * (0.75f + 0.25f * v), 0.0f, 1.25f));
    } else {
      contactDelaySamples[(size_t)note].fill(0);
    }

    ampsDirty = true;

    // Single-trigger B3 harmonic percussion: fires only on 0 -> 1 held key
    if (activeNoteCount == 1 && percEnabled) {
      percEnv = 1.0f;
      percActive = true;
    }
  }

  void noteOff(int note) {
    note &= 127;
    if (!held.test((size_t)note))
      return;
    held.reset((size_t)note);
    auto &cd = contactDelaySamples[(size_t)note];
    for (int d = 0; d < kNumDrawbars; ++d) {
      if (cd[(size_t)d] > 0) {
        cd[(size_t)d] = 0;
        if (staggerActiveCount > 0)
          --staggerActiveCount;
      }
    }
    activeNoteCount = activeNoteCount > 0 ? activeNoteCount - 1 : 0;
    ampsDirty = true;

    if (clickDepth > 0.01f) {
      woodEnergy  = juceMax(woodEnergy,  0.14f * clickDepth);
      sparkEnergy = juceMax(sparkEnergy, 0.18f * clickDepth);
    }
  }

  void panic() {
    held.reset();
    for (auto &arr : contactDelaySamples)
      arr.fill(0);
    staggerActiveCount = 0;
    activeNoteCount = 0;
    percActive = false;
    percEnv = 0.0f;
    woodEnergy = sparkEnergy = 0.0f;
    wheelTargetAmp.fill(0.0f);
    percWheelTargetAmp.fill(0.0f);
    ampsDirty = false;
  }

  void setDrawbar(int idx, float v) {
    if (idx < 0 || idx >= kNumDrawbars)
      return;
    drawbar[(size_t)idx] = juceClamp(v, 0.0f, 1.0f);
    ampsDirty = true;
  }
  float getDrawbar(int idx) const {
    return (idx >= 0 && idx < kNumDrawbars) ? drawbar[(size_t)idx] : 0.0f;
  }

  void setPercussion(bool on, bool soft, bool fast, bool third) {
    const bool wasEnabled = percEnabled;
    percEnabled = on;
    percSoft = soft;
    percFast = fast;
    percThird = third;
    if (!on) {
      percActive = false;
      percEnv = 0.0f;
    } else if (!wasEnabled && activeNoteCount > 0) {
      percEnv = 1.0f;
      percActive = true;
    }
    ampsDirty = true;
  }

  void setInvertDrawbars(bool) {}
  void setLeakage(float v) {
    leakage = juceClamp(v, 0.0f, 0.066f);
    ampsDirty = true;
  }
  void setClickDepth(float v) { clickDepth = juceClamp(v, 0.0f, 1.0f); }
  int activeVoices() const { return activeNoteCount; }

  // Combined mono render (used by unit tests / simple callers)
  void render(float *out, int n) { renderSplit(out, out, n); }

  // Split render: main drawbars + key-click -> drawbarOut (feeds Scanner Chorus);
  // Harmonic Percussion -> percOut (bypasses Scanner Chorus like a real stock B3!).
  // Both buffers are added-to (caller clears them beforehand). May alias safely.
  void renderSplit(float *drawbarOut, float *percOut, int n) {
    if (n <= 0)
      return;
    if (ampsDirty) {
      rebuildWheelAmps();
      ampsDirty = false;
    }

    int offset = 0;
    while (offset < n) {
      // Step in 32-sample (~0.67 ms) slices while busbar contacts are closing,
      // otherwise run fast 128-sample chunks.
      const int maxSlice = (staggerActiveCount > 0) ? 32 : 128;
      const int m = (n - offset) > maxSlice ? maxSlice : (n - offset);

      if (staggerActiveCount > 0)
        advanceContactStagger(m);

      renderChunk(drawbarOut + offset, percOut + offset, m);
      offset += m;
    }
  }

  void triggerClick(float energy) {
    const float e = juceClamp(energy, 0.0f, 1.25f);
    woodEnergy  = juceMax(woodEnergy, e);
    sparkEnergy = juceMax(sparkEnergy, e * 0.85f);
  }

  static float juceMax(float a, float b) { return a > b ? a : b; }
  static float juceClamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  }

private:
  static constexpr int kLutSize = 4096;
  std::array<float, kLutSize> lutSub{};
  std::array<float, kLutSize> lutLowMid{};
  std::array<float, kLutSize> lutUpperMid{};
  std::array<float, kLutSize> lutHigh{};

  struct SvfBandpass {
    float a1 = 0.1f, a2 = 0.02f, a3 = 0.004f;
    float ic1 = 0.0f, ic2 = 0.0f;
  };

  static void setupSvf(SvfBandpass &f, float fc, float q, float srF) {
    const float g = std::tan(3.14159265f * fc / srF);
    const float k = 1.0f / q;
    f.a1 = 1.0f / (1.0f + g * (g + k));
    f.a2 = g * f.a1;
    f.a3 = g * f.a2;
  }

  static inline float stepSvfBp(SvfBandpass &f, float v0) noexcept {
    const float v3 = v0 - f.ic2;
    const float v1 = f.a1 * f.ic1 + f.a2 * v3;
    const float v2 = f.ic2 + f.a2 * f.ic1 + f.a3 * v3;
    f.ic1 = 2.0f * v1 - f.ic1;
    f.ic2 = 2.0f * v2 - f.ic2;
    return v1;
  }

  void advanceContactStagger(int elapsedSamples) {
    bool anyJustClosed = false;
    for (int note = 0; note < 128; ++note) {
      if (!held.test((size_t)note))
        continue;
      auto &cd = contactDelaySamples[(size_t)note];
      for (int d = 0; d < kNumDrawbars; ++d) {
        if (cd[(size_t)d] > 0) {
          cd[(size_t)d] -= elapsedSamples;
          if (cd[(size_t)d] <= 0) {
            cd[(size_t)d] = 0;
            if (staggerActiveCount > 0)
              --staggerActiveCount;
            anyJustClosed = true;
          }
        }
      }
    }
    if (anyJustClosed)
      rebuildWheelAmps();
  }

  inline const float *lutForWheel(int w) const noexcept {
    if (w < 12) return lutSub.data();
    if (w < 36) return lutLowMid.data();
    if (w < 60) return lutUpperMid.data();
    return lutHigh.data();
  }

  void renderChunk(float *drawbarOut, float *percOut, int m) {
    float pEnvGain[128];
    const bool hasPercEnv = percActive && percEnv > 1.0e-5f;
    if (hasPercEnv) {
      // Punchy B3 tube-gated harmonic percussion level (Normal has bold wooden ping)
      const float percLevel = percSoft ? 0.52f : 1.15f;
      const float decay = (activeNoteCount == 0)
                              ? percDecayRel
                              : (percFast ? percDecayFast : percDecaySlow);
      float pe = percEnv;
      for (int s = 0; s < m; ++s) {
        if (pe >= 1.0e-4f) {
          // Subtle initial diode-bridge snap on first few milliseconds
          const float snap = 1.0f + 0.22f * pe * pe;
          pEnvGain[s] = pe * percLevel * snap;
          pe *= decay;
        } else {
          pe = 0.0f;
          pEnvGain[s] = 0.0f;
        }
      }
      percEnv = pe;
      if (percEnv < 1.0e-4f) {
        percEnv = 0.0f;
        percActive = false;
      }
    }

    const float fm = (float)m;
      // Update 12-bin synchronous shaft & spring-coupling micro-flutter (~0.42 cents):
      // Keeps octaves and fifths organically breathing in phase without audible pitch drift.
      shaftFlutterPhaseA += 1.73f * fm / (float)sr;
      if (shaftFlutterPhaseA >= 1.0f) shaftFlutterPhaseA -= 1.0f;
      shaftFlutterPhaseB += 4.31f * fm / (float)sr;
      if (shaftFlutterPhaseB >= 1.0f) shaftFlutterPhaseB -= 1.0f;
      float binFlutter[12];
      for (int b = 0; b < 12; ++b) {
        const float phA = 6.2831853f * (shaftFlutterPhaseA + (float)b * (1.0f / 12.0f));
        const float phB = 6.2831853f * (shaftFlutterPhaseB + (float)(b * 5 % 12) * (1.0f / 12.0f));
        binFlutter[b] = 1.0f + 0.00024f * (0.68f * std::sin(phA) + 0.32f * std::sin(phB));
      }

    for (int w = 0; w < kNumWheels; ++w) {
      const float tgt = wheelTargetAmp[(size_t)w];
      float cur = wheelCurAmp[(size_t)w];
      const float ptgt = percWheelTargetAmp[(size_t)w];
      float pcur = percWheelCurAmp[(size_t)w];

      const bool wheelSilent = (tgt == 0.0f && cur < 1.0e-6f);
      const bool percSilent = (!hasPercEnv) || (ptgt == 0.0f && pcur < 1.0e-6f);

      if (wheelSilent && percSilent) {
        wheelCurAmp[(size_t)w] = 0.0f;
        if (!hasPercEnv || ptgt == 0.0f)
          percWheelCurAmp[(size_t)w] = 0.0f;
        float p = phase[(size_t)w] + wheelPhaseInc[(size_t)w] * binFlutter[w % 12] * fm;
        phase[(size_t)w] = p - std::floor(p);
        continue;
      }

      const float * __restrict lut = lutForWheel(w);
      float ph = phase[(size_t)w];
      const float inc = wheelPhaseInc[(size_t)w] * binFlutter[w % 12];
      const float curCoef = (tgt > cur) ? wheelAtk : wheelRel;

      constexpr double kQ32Scale = 4294967296.0;
      uint32_t ph32 = (uint32_t)(ph * kQ32Scale);
      const uint32_t inc32 = (uint32_t)(inc * kQ32Scale);
      constexpr float kFracScale = 1.0f / 1048576.0f;

      if (percSilent) {
        if (std::abs(tgt - cur) < 1.0e-5f) {
          cur = tgt;
          for (int s = 0; s < m; ++s) {
            ph32 += inc32;
            const uint32_t i0 = ph32 >> 20;
            const uint32_t i1 = (i0 + 1u) & (kLutSize - 1u);
            const float frac = (float)(ph32 & 0xFFFFFu) * kFracScale;
            const float y0 = lut[i0];
            drawbarOut[s] += cur * (y0 + frac * (lut[i1] - y0));
          }
        } else {
          for (int s = 0; s < m; ++s) {
            ph32 += inc32;
            cur += curCoef * (tgt - cur);
            const uint32_t i0 = ph32 >> 20;
            const uint32_t i1 = (i0 + 1u) & (kLutSize - 1u);
            const float frac = (float)(ph32 & 0xFFFFFu) * kFracScale;
            const float y0 = lut[i0];
            drawbarOut[s] += cur * (y0 + frac * (lut[i1] - y0));
          }
        }
        pcur = (ptgt == 0.0f) ? 0.0f : ptgt;
      } else {
        const float pcurCoef = (ptgt > pcur) ? wheelAtk : wheelRel;
        for (int s = 0; s < m; ++s) {
          ph32 += inc32;
          cur += curCoef * (tgt - cur);
          pcur += pcurCoef * (ptgt - pcur);
          const uint32_t i0 = ph32 >> 20;
          const uint32_t i1 = (i0 + 1u) & (kLutSize - 1u);
          const float frac = (float)(ph32 & 0xFFFFFu) * kFracScale;
          const float y0 = lut[i0];
          const float sig = y0 + frac * (lut[i1] - y0);
          drawbarOut[s] += cur * sig;
          percOut[s] += (pEnvGain[s] * pcur) * sig;
        }
      }

      float pBack = (float)((double)ph32 * (1.0 / kQ32Scale));
      phase[(size_t)w] = pBack >= 1.0f ? 0.0f : pBack;
      wheelCurAmp[(size_t)w] = cur;
      percWheelCurAmp[(size_t)w] = pcur;
    }

    // Dual-band woody + palladium contact key-click transient
    if ((woodEnergy > 1.0e-5f || sparkEnergy > 1.0e-5f) && clickDepth > 0.005f) {
      const float gain = 0.42f * clickDepth;
      for (int s = 0; s < m; ++s) {
        if (woodEnergy <= 1.0e-5f && sparkEnergy <= 1.0e-5f) {
          woodEnergy = sparkEnergy = 0.0f;
          break;
        }
        woodEnergy *= clickWoodDecay;
        sparkEnergy *= clickSparkDecay;
        const float n1 = (xorShift() - 0.5f) * 2.0f;
        // Brief crackly palladium contact bounce
        const float bounce = (n1 > 0.42f ? 1.35f : (n1 < -0.42f ? -1.1f : 0.15f * n1));
        const float wood  = stepSvfBp(woodSvf,  (n1 * 0.75f + 0.30f) * woodEnergy);
        const float spark = stepSvfBp(sparkSvf, bounce * sparkEnergy);
        drawbarOut[s] += (0.68f * wood + 0.52f * spark) * gain;
      }
    }
  }

  std::array<float, kNumWheels> phase{};
  std::array<float, kNumWheels> wheelFreq{};
  std::array<float, kNumWheels> wheelPhaseInc{};
  std::array<float, kNumWheels> wheelPickupGain{};
  std::array<float, kNumWheels> wheelTargetAmp{};
  std::array<float, kNumWheels> wheelCurAmp{};
  std::array<float, kNumWheels> percWheelTargetAmp{};
  std::array<float, kNumWheels> percWheelCurAmp{};
  std::array<float, kNumDrawbars> drawbar{};
  std::array<std::array<int, kNumDrawbars>, 128> contactDelaySamples{};
  int staggerActiveCount = 0;

  uint32_t rngState = 0x13572468u;
  double sr = 44100.0;

  bool percEnabled = false, percSoft = true, percFast = false, percThird = true;
  bool percActive = false;
  float percEnv = 0.0f;
  float percDecayFast = 0.9992f;
  float percDecaySlow = 0.9997f;
  float percDecayRel = 0.994f;

  float wheelAtk = 0.02f;
  float wheelRel = 0.01f;

  float woodEnergy = 0.0f, sparkEnergy = 0.0f;
  float clickWoodDecay = 0.99f, clickSparkDecay = 0.98f;
  SvfBandpass woodSvf{}, sparkSvf{};
  float clickDepth = 0.5f;

  float leakage = 0.018f;
  float shaftFlutterPhaseA = 0.0f;
  float shaftFlutterPhaseB = 0.37f;
  int activeNoteCount = 0;
  bool ampsDirty = true;
  std::bitset<128> held;

  void buildLuts() {
    constexpr float kTwoPi = 6.28318530717958647692f;
    // Electromagnetic pickup reluctance model: spinning steel teeth passing a
    // permanent magnet pole-piece induce dPhi/dt ~ sin(th) / (1 - ecc*cos(th))^2,
    // giving real B3 tonewheels warm crest asymmetry and subtle iron overtones.
    auto reluctanceWave = [](float th, float ecc) -> float {
      const float denom = 1.0f - ecc * std::cos(th + 0.35f);
      return std::sin(th) * ((1.0f - ecc * ecc) / (denom * denom));
    };

    for (int i = 0; i < kLutSize; ++i) {
      const float th = (float)i * (kTwoPi / (float)kLutSize);
      const float s2 = std::sin(2.0f * th + 0.28f);
      const float s3 = std::sin(3.0f * th + 0.08f);
      const float s4 = std::sin(4.0f * th + 0.62f);
      const float s5 = std::sin(5.0f * th);

      // Sub-wheels 0..11: complex 2/4-tooth scalloped Hammond bass tonewheels
      const float rSub = reluctanceWave(th, 0.13f);
      lutSub[(size_t)i] =
          (0.84f * rSub + 0.16f * s2 + 0.24f * s3 + 0.065f * s4 + 0.048f * s5) * 0.86f;

      // Low-mid wheels 12..35 (C2..B3): warm magnetic steel-tooth pickup iron
      const float rLow = reluctanceWave(th, 0.085f);
      lutLowMid[(size_t)i] =
          (0.92f * rLow + 0.068f * s2 + 0.045f * s3 - 0.012f * s4) * 0.96f;

      // Upper-mid wheels 36..59 (C4..B5): subtle pickup-coil reluctance asymmetry
      const float rMid = reluctanceWave(th, 0.048f);
      lutUpperMid[(size_t)i] = (0.96f * rMid + 0.032f * s2 + 0.016f * s3) * 0.985f;

      // Top wheels 60..90: LC-transformer filtered 128/192-tooth wheels
      const float rHi = reluctanceWave(th, 0.018f);
      lutHigh[(size_t)i] = (0.99f * rHi + 0.009f * s2) * 0.995f;
    }
  }

  static float drawbarTapGain(float norm) {
    static constexpr float kTap[9] = {
        0.0f, 0.095f, 0.145f, 0.21f, 0.30f, 0.42f, 0.57f, 0.76f, 1.00f};
    const float f = juceClamp(norm, 0.0f, 1.0f) * 8.0f;
    const int i0 = (int)f;
    if (i0 >= 8)
      return 1.0f;
    const float frac = f - (float)i0;
    return kTap[i0] + frac * (kTap[i0 + 1] - kTap[i0]);
  }

  static int mapNoteDrawbarToWheel(int note, int drawbarIdx) {
    int w = note + kDrawbarSemitones[drawbarIdx] - 24;
    while (w >= kNumWheels)
      w -= 12;
    if (note >= 36) {
      while (w < 12)
        w += 12;
    } else {
      while (w < 0)
        w += 12;
    }
    return (w >= 0 && w < kNumWheels) ? w : -1;
  }

  float xorShift() {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return (float)(rngState & 0xffffffu) * (1.0f / 16777216.0f);
  }

  void rebuildWheelAmps() {
    wheelTargetAmp.fill(0.0f);
    percWheelTargetAmp.fill(0.0f);
    if (activeNoteCount <= 0)
      return;

    float dbGain[kNumDrawbars];
    const float percPad = (percEnabled && !percSoft) ? 0.76f : 1.0f;
    for (int i = 0; i < kNumDrawbars; ++i)
      dbGain[i] = drawbarTapGain(drawbar[(size_t)i]) * percPad;
    if (percEnabled)
      dbGain[8] = 0.0f; // Classic 1' cancel when Percussion is engaged

    // Realistic matching-transformer impedance robbing on big chords
    const float polyComp =
        0.072f / std::sqrt(1.0f + 0.085f * juceMax(0.0f, (float)(activeNoteCount - 1)));
    const int percDbIdx = percThird ? 4 : 3;

    for (int note = 0; note < 128; ++note) {
      if (!held.test((size_t)note))
        continue;
      const auto &cd = contactDelaySamples[(size_t)note];
      for (int d = 0; d < kNumDrawbars; ++d) {
        if (dbGain[d] <= 0.0f || cd[(size_t)d] > 0)
          continue;
        const int w = mapNoteDrawbarToWheel(note, d);
        if (w >= 0) {
          // Busbar robbed slightly when multiple held keys share the same wheel
          const float existing = wheelTargetAmp[(size_t)w];
          const float rob = (existing > 0.001f) ? 0.62f : 1.0f;
          wheelTargetAmp[(size_t)w] +=
              dbGain[d] * wheelPickupGain[(size_t)w] * polyComp * rob;
        }
      }
      if (percEnabled) {
        const int pw = mapNoteDrawbarToWheel(note, percDbIdx);
        if (pw >= 0)
          percWheelTargetAmp[(size_t)pw] +=
              wheelPickupGain[(size_t)pw] * polyComp;
      }
    }

    // Authentic B3 generator compartment & wiring harness crosstalk matrix:
    // - +/-48 wheels: physical 4-octave companion wheel in the same bin
    // - +/-12, +/-19: sympathetic octave/fifth busbar induction
    // - +/-1, +/-4: adjacent shaft & wiring harness bleed
    if (leakage > 0.0001f) {
      std::array<float, kNumWheels> dryCopy = wheelTargetAmp;
      const float bleed = leakage * 0.55f;
      for (int w = 0; w < kNumWheels; ++w) {
        const float src = dryCopy[(size_t)w];
        if (src <= 0.0004f)
          continue;
        const float b = src * bleed;
        if (w - 48 >= 0)          wheelTargetAmp[(size_t)(w - 48)] += b * 1.15f;
        if (w + 48 < kNumWheels)  wheelTargetAmp[(size_t)(w + 48)] += b * 0.95f;
        if (w - 12 >= 0)          wheelTargetAmp[(size_t)(w - 12)] += b * 0.55f;
        if (w + 12 < kNumWheels)  wheelTargetAmp[(size_t)(w + 12)] += b * 0.55f;
        if (w + 19 < kNumWheels)  wheelTargetAmp[(size_t)(w + 19)] += b * 0.35f;
        if (w - 1 >= 0)           wheelTargetAmp[(size_t)(w - 1)]  += b * 0.70f;
        if (w + 1 < kNumWheels)   wheelTargetAmp[(size_t)(w + 1)]  += b * 0.70f;
        if (w - 4 >= 0)           wheelTargetAmp[(size_t)(w - 4)]  += b * 0.45f;
        if (w + 4 < kNumWheels)   wheelTargetAmp[(size_t)(w + 4)]  += b * 0.45f;
      }
    }
  }
};

} // namespace B3
