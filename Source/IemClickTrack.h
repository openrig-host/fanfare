#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cmath>

namespace Fanfare {

/**
 * Lock-free, allocation-free synthesized metronome / count-in mixed
 * strictly into the IEM bus (never FOH).
 */
class IemClickTrack {
public:
  enum Mode : int { Continuous = 0, CountIn1Bar = 1, CountIn2Bars = 2 };

  void prepare(double sampleRate) {
    sr = sampleRate > 8000.0 ? sampleRate : 44100.0;
    resetPhase();
  }

  void setEnabled(bool on) {
    const bool prev = enabled.exchange(on, std::memory_order_acq_rel);
    if (on && !prev)
      restartPending.store(true, std::memory_order_release);
  }
  bool isEnabled() const noexcept { return enabled.load(std::memory_order_relaxed); }

  void setBpm(float newBpm) {
    bpm.store(juce::jlimit(30.0f, 260.0f, newBpm), std::memory_order_relaxed);
  }
  float getBpm() const noexcept { return bpm.load(std::memory_order_relaxed); }

  void setBeatsPerBar(int beats) {
    beatsPerBar.store(juce::jlimit(1, 12, beats), std::memory_order_relaxed);
  }
  int getBeatsPerBar() const noexcept { return beatsPerBar.load(std::memory_order_relaxed); }

  void setMode(int m) {
    mode.store(juce::jlimit(0, 2, m), std::memory_order_relaxed);
  }
  int getMode() const noexcept { return mode.load(std::memory_order_relaxed); }

  void setLevel(float v) {
    level.store(juce::jlimit(0.0f, 1.0f, v), std::memory_order_relaxed);
  }
  float getLevel() const noexcept { return level.load(std::memory_order_relaxed); }

  int getCurrentBeat() const noexcept { return currentBeat.load(std::memory_order_relaxed); }

  void resetPhase() noexcept {
    sampleInBeat = 0;
    beatIndex = 0;
    totalBeatsPlayed = 0;
    env = 0.0f;
    phase1 = 0.0f;
    phase2 = 0.0f;
    currentBeat.store(0, std::memory_order_relaxed);
  }

  // Mixes click strictly into iemBus (stereo/mono). Never touches FOH.
  void processIntoIem(juce::AudioBuffer<float> &iemBus, int numSamples) noexcept {
    if (numSamples <= 0 || iemBus.getNumChannels() <= 0)
      return;

    if (restartPending.exchange(false, std::memory_order_acq_rel)) {
      resetPhase();
      triggerTick(true);
    }

    if (!enabled.load(std::memory_order_relaxed))
      return;

    const float curBpm = juce::jlimit(30.0f, 260.0f, bpm.load(std::memory_order_relaxed));
    const int barBeats = juce::jlimit(1, 12, beatsPerBar.load(std::memory_order_relaxed));
    const int curMode = mode.load(std::memory_order_relaxed);
    const float gain = level.load(std::memory_order_relaxed) * 0.45f;

    const int samplesPerBeat = juce::jmax(64, (int)std::lround(sr * 60.0 / (double)curBpm));
    const float srF = (float)sr;
    const float decay = std::exp(-1.0f / ((beatIndex == 0 ? 0.018f : 0.013f) * srF));

    float *ch0 = iemBus.getWritePointer(0);
    float *ch1 = iemBus.getNumChannels() > 1 ? iemBus.getWritePointer(1) : nullptr;

    for (int s = 0; s < numSamples; ++s) {
      if (sampleInBeat >= samplesPerBeat) {
        sampleInBeat = 0;
        ++totalBeatsPlayed;
        if (curMode != Continuous) {
          const int maxBeats = barBeats * (curMode == CountIn2Bars ? 2 : 1);
          if (totalBeatsPlayed >= maxBeats) {
            enabled.store(false, std::memory_order_relaxed);
            currentBeat.store(0, std::memory_order_relaxed);
            break;
          }
        }
        beatIndex = (beatIndex + 1) % barBeats;
        triggerTick(beatIndex == 0);
      }

      if (env > 1.0e-5f) {
        phase1 += inc1;
        if (phase1 >= 1.0f) phase1 -= 1.0f;
        phase2 += inc2;
        if (phase2 >= 1.0f) phase2 -= 1.0f;

        constexpr float kTwoPi = 6.28318530717958647692f;
        const float sig = (0.72f * std::sin(kTwoPi * phase1) +
                           0.28f * std::sin(kTwoPi * phase2)) *
                          env * gain;
        ch0[s] += sig;
        if (ch1 != nullptr)
          ch1[s] += sig;
        env *= decay;
      }

      ++sampleInBeat;
    }
  }

private:
  void triggerTick(bool accent) noexcept {
    const float srF = (float)sr;
    const float f1 = accent ? 1680.0f : 1080.0f;
    const float f2 = accent ? 2520.0f : 1620.0f;
    inc1 = f1 / srF;
    inc2 = f2 / srF;
    phase1 = 0.0f;
    phase2 = 0.0f;
    env = accent ? 1.0f : 0.68f;
    currentBeat.store(beatIndex + 1, std::memory_order_relaxed);
  }

  double sr = 44100.0;
  std::atomic<bool> enabled{false};
  std::atomic<bool> restartPending{false};
  std::atomic<float> bpm{120.0f};
  std::atomic<int> beatsPerBar{4};
  std::atomic<int> mode{Continuous};
  std::atomic<float> level{0.55f};
  std::atomic<int> currentBeat{0};

  int sampleInBeat = 0;
  int beatIndex = 0;
  int totalBeatsPlayed = 0;
  float env = 0.0f;
  float phase1 = 0.0f, phase2 = 0.0f;
  float inc1 = 0.0f, inc2 = 0.0f;
};

} // namespace Fanfare
