#pragma once

#include <JuceHeader.h>
#include "Logger.h"
#include <atomic>
#include <cmath>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <powerbase.h>
#include <powrprof.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "pdh.lib")
#endif

/**
 * Low-overhead background CPU thermal & firmware-throttle monitor.
 *
 * Polls Windows user-mode power/thermal APIs every 2 seconds on a low-priority
 * background thread (never on the audio or UI thread) with zero admin rights:
 *   1. CallNtPowerInformation(ProcessorInformation) -> MhzLimit vs MaxMhz
 *      (detects exact BIOS/EC firmware throttling behind Windows Event ID 37,
 *      such as choked bottom vents, skin-temp lap clamp, or derated charger).
 *   2. PDH ACPI Thermal Zone temperature & % Passive Limit (\_TZ.THM0).
 *   3. PDH % Processor Performance & % of Maximum Frequency vs audio DSP load.
 */
class ThermalMonitor : private juce::Thread {
public:
  struct Snapshot {
    bool warningActive = false;     // True while actively throttled or within 20s latch
    bool activelyThrottled = false; // True on the most recent 2s sample
    int tempCelsius = -1;           // -1 if ACPI thermal zone unavailable
    int peakTempCelsius = -1;
    int nominalMaxMhz = 0;
    int minMhzLimit = 0;
    int effectiveMhz = 0;
    int osTurboCapMhz = 0;
    int passiveLimitPct = 100;
    int pctOfMaxFreq = 100;
    juce::String badgeText;         // Short text for top bar, e.g. "THERMAL 92C" or "THERMAL"
    juce::String reasonText;        // Human-readable reason for tooltip/dialog/logs
  };

  ThermalMonitor() : juce::Thread("FanfareThermalMonitor") {}

  ~ThermalMonitor() override { stopMonitoring(); }

  void startMonitoring(uintptr_t bgAffinityMask = 0) {
    backgroundMask.store(bgAffinityMask, std::memory_order_relaxed);
    if (!isThreadRunning())
      startThread(juce::Thread::Priority::low);
  }

  void stopMonitoring() {
    signalThreadShouldExit();
    notify();
    stopThread(2500);
  }

  void setBackgroundAffinityMask(uintptr_t mask) {
    backgroundMask.store(mask, std::memory_order_relaxed);
  }

  void setAudioCpuLoadPercent(double pct) {
    audioCpuPct.store((float)pct, std::memory_order_relaxed);
  }

  void acknowledgeLatch() {
    latchedUntilMs.store(0, std::memory_order_relaxed);
    warningActiveFlag.store(activelyThrottledFlag.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
  }

  bool isWarningActive() const {
    return warningActiveFlag.load(std::memory_order_relaxed);
  }

  int getTemperatureCelsius() const {
    return currentTempC.load(std::memory_order_relaxed);
  }

  Snapshot getSnapshot() const {
    Snapshot s;
    s.warningActive = warningActiveFlag.load(std::memory_order_relaxed);
    s.activelyThrottled = activelyThrottledFlag.load(std::memory_order_relaxed);
    s.tempCelsius = currentTempC.load(std::memory_order_relaxed);
    s.peakTempCelsius = peakTempC.load(std::memory_order_relaxed);
    s.nominalMaxMhz = nominalMhz.load(std::memory_order_relaxed);
    s.minMhzLimit = mhzLimit.load(std::memory_order_relaxed);
    s.effectiveMhz = effectiveClockMhz.load(std::memory_order_relaxed);
    s.osTurboCapMhz = osFreqCapAtom.load(std::memory_order_relaxed);
    s.passiveLimitPct = passivePct.load(std::memory_order_relaxed);
    s.pctOfMaxFreq = maxFreqPct.load(std::memory_order_relaxed);

    if (s.tempCelsius > 0)
      s.badgeText = "THERMAL " + juce::String(s.tempCelsius) + "C";
    else
      s.badgeText = "THERMAL";

    const juce::ScopedLock sl(reasonLock);
    s.reasonText = lastReason;
    return s;
  }

  juce::String formatGlitchLogTag() const {
    const int t = currentTempC.load(std::memory_order_relaxed);
    const bool warn = warningActiveFlag.load(std::memory_order_relaxed);
    const int eff = effectiveClockMhz.load(std::memory_order_relaxed);
    const int lim = mhzLimit.load(std::memory_order_relaxed);
    const int nom = nominalMhz.load(std::memory_order_relaxed);

    juce::String out;
    if (t > 0)
      out << "Temp: " << t << "C";
    else
      out << "Temp: n/a";

    if (eff > 0)
      out << " (" << eff << "MHz)";

    if (warn) {
      out << " [THERMAL/THROTTLED";
      if (nom > 0 && lim > 0 && lim < nom)
        out << " Limit=" << lim << "/" << nom << "MHz";
      out << "]";
    }
    return out;
  }

private:
#ifdef _WIN32
  // Matches Windows kernel PROCESSOR_POWER_INFORMATION layout (omitted from winnt.h)
  struct WinProcessorPowerInfo {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
  };
#endif

  static constexpr juce::int64 kLatchDurationMs = 20000;
  static constexpr int kHighTempTriggerC = 90;
  static constexpr int kHighTempClearC = 85;

  std::atomic<uintptr_t> backgroundMask{0};
  std::atomic<float> audioCpuPct{0.0f};

  std::atomic<bool> warningActiveFlag{false};
  std::atomic<bool> activelyThrottledFlag{false};
  std::atomic<juce::int64> latchedUntilMs{0};

  std::atomic<int> currentTempC{-1};
  std::atomic<int> peakTempC{-1};
  std::atomic<int> nominalMhz{0};
  std::atomic<int> mhzLimit{0};
  std::atomic<int> effectiveClockMhz{0};
  std::atomic<int> osFreqCapAtom{0};
  std::atomic<int> passivePct{100};
  std::atomic<int> maxFreqPct{100};

  mutable juce::CriticalSection reasonLock;
  juce::String lastReason{"Nominal"};

  void run() override {
#ifdef _WIN32
    uintptr_t appliedMask = 0;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER tempCounter = nullptr;
    PDH_HCOUNTER passiveCounter = nullptr;
    PDH_HCOUNTER perfPctCounter = nullptr;
    PDH_HCOUNTER maxFreqPctCounter = nullptr;

    if (::PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
      if (::PdhAddEnglishCounterW(
              query, L"\\Thermal Zone Information(*)\\Temperature", 0,
              &tempCounter) != ERROR_SUCCESS) {
        tempCounter = nullptr;
      }
      if (::PdhAddEnglishCounterW(
              query, L"\\Thermal Zone Information(*)\\% Passive Limit", 0,
              &passiveCounter) != ERROR_SUCCESS) {
        passiveCounter = nullptr;
      }
      if (::PdhAddEnglishCounterW(
              query, L"\\Processor Information(_Total)\\% Processor Performance",
              0, &perfPctCounter) != ERROR_SUCCESS) {
        perfPctCounter = nullptr;
      }
      if (::PdhAddEnglishCounterW(
              query, L"\\Processor Information(_Total)\\% of Maximum Frequency",
              0, &maxFreqPctCounter) != ERROR_SUCCESS) {
        maxFreqPctCounter = nullptr;
      }
      // Prime rate-based counters once
      ::PdhCollectQueryData(query);
    }

    SYSTEM_INFO sysInfo{};
    ::GetSystemInfo(&sysInfo);
    const int numProcs = juce::jlimit(1, 128, (int)sysInfo.dwNumberOfProcessors);
    std::vector<WinProcessorPowerInfo> pwrBuf((size_t)numProcs);

    bool wasOverTemp = false;
    bool wasLoggedThrottle = false;
    int lastObservedOsCapMhz = -1;
    int healthyOsLimitMhz = 0;

    while (!threadShouldExit()) {
      const uintptr_t wantMask = backgroundMask.load(std::memory_order_relaxed);
      if (wantMask != 0 && wantMask != appliedMask) {
        appliedMask = wantMask;
        ::SetThreadAffinityMask(::GetCurrentThread(), (DWORD_PTR)wantMask);
      }

      // 0. Check whether Windows power plan has an intentional turbo cap (PROCFREQMAX / PROCTHROTTLEMAX)
      // On Intel Speed Shift / CPPC, an OS turbo cap (e.g. 2800/3600 MHz out of 4400 MHz turbo)
      // scales PROCESSOR_POWER_INFORMATION.MhzLimit proportionally against NominalMhz (e.g. 1040/1600),
      // even though the CPU is unthrottled and boosting cleanly above base clock.
      int activeOsCapMhz = 0;
      int activeOsThrottlePct = 100;
      readActiveOsFrequencyCap(activeOsCapMhz, activeOsThrottlePct);
      osFreqCapAtom.store(activeOsCapMhz, std::memory_order_relaxed);
      const bool hasIntentionalOsCap =
          (activeOsCapMhz > 0) || (activeOsThrottlePct > 0 && activeOsThrottlePct < 100);
      if (activeOsCapMhz != lastObservedOsCapMhz) {
        lastObservedOsCapMhz = activeOsCapMhz;
        healthyOsLimitMhz = 0;
      }

      // 1. Kernel per-core MhzLimit vs MaxMhz (< 5 us)
      int sampleNominalMhz = 0;
      int sampleMinLimitMhz = 0;
      const ULONG bufBytes = (ULONG)(pwrBuf.size() * sizeof(WinProcessorPowerInfo));
      if (::CallNtPowerInformation(ProcessorInformation, nullptr, 0,
                                   pwrBuf.data(), bufBytes) == 0) {
        for (int i = 0; i < numProcs; ++i) {
          const int mx = (int)pwrBuf[(size_t)i].MaxMhz;
          const int lim = (int)pwrBuf[(size_t)i].MhzLimit;
          if (mx > sampleNominalMhz)
            sampleNominalMhz = mx;
          if (lim > 0 && (sampleMinLimitMhz == 0 || lim < sampleMinLimitMhz))
            sampleMinLimitMhz = lim;
        }
      }
      nominalMhz.store(sampleNominalMhz, std::memory_order_relaxed);
      mhzLimit.store(sampleMinLimitMhz, std::memory_order_relaxed);

      if (hasIntentionalOsCap && sampleMinLimitMhz > healthyOsLimitMhz) {
        healthyOsLimitMhz = sampleMinLimitMhz;
      }

      // 2. ACPI Thermal Zone & CPU performance counters
      int sampleTempC = -1;
      int samplePassivePct = 100;
      int sampleMaxFreqPct = 100;
      int sampleEffectiveMhz = 0;

      if (query != nullptr && ::PdhCollectQueryData(query) == ERROR_SUCCESS) {
        if (tempCounter != nullptr) {
          const double maxKelvin = readMaxCounterArrayValue(tempCounter);
          if (maxKelvin >= 250.0 && maxKelvin <= 420.0) {
            sampleTempC = (int)std::round(maxKelvin - 273.15);
          }
        }
        if (passiveCounter != nullptr) {
          const double minPassive = readMinCounterArrayValue(passiveCounter);
          if (minPassive > 0.0 && minPassive <= 100.0) {
            samplePassivePct = (int)std::round(minPassive);
          }
        }
        if (perfPctCounter != nullptr) {
          PDH_FMT_COUNTERVALUE val{};
          if (::PdhGetFormattedCounterValue(perfPctCounter, PDH_FMT_DOUBLE,
                                            nullptr, &val) == ERROR_SUCCESS &&
              val.CStatus == ERROR_SUCCESS && sampleNominalMhz > 0) {
            sampleEffectiveMhz =
                (int)std::round((double)sampleNominalMhz * (val.doubleValue / 100.0));
          }
        }
        if (maxFreqPctCounter != nullptr) {
          PDH_FMT_COUNTERVALUE val{};
          if (::PdhGetFormattedCounterValue(maxFreqPctCounter, PDH_FMT_LONG,
                                            nullptr, &val) == ERROR_SUCCESS &&
              val.CStatus == ERROR_SUCCESS && val.longValue > 0) {
            sampleMaxFreqPct = (int)val.longValue;
          }
        }
      }

      if (sampleTempC > 0) {
        currentTempC.store(sampleTempC, std::memory_order_relaxed);
        int prevPeak = peakTempC.load(std::memory_order_relaxed);
        if (sampleTempC > prevPeak)
          peakTempC.store(sampleTempC, std::memory_order_relaxed);
      }
      passivePct.store(samplePassivePct, std::memory_order_relaxed);
      maxFreqPct.store(sampleMaxFreqPct, std::memory_order_relaxed);
      if (sampleEffectiveMhz > 0)
        effectiveClockMhz.store(sampleEffectiveMhz, std::memory_order_relaxed);

      // 3. Evaluate true thermal / firmware throttle conditions (ignoring intentional OS turbo caps)
      if (sampleTempC >= kHighTempTriggerC)
        wasOverTemp = true;
      else if (sampleTempC > 0 && sampleTempC <= kHighTempClearC)
        wasOverTemp = false;

      bool firmwareClamped = false;
      if (sampleNominalMhz > 0 && sampleMinLimitMhz > 0) {
        if (!hasIntentionalOsCap) {
          firmwareClamped = (sampleMinLimitMhz < (sampleNominalMhz * 95) / 100);
        } else {
          // OS turbo cap is active: only alert if BIOS/EC pushes MhzLimit down below the OS cap
          // (Event ID 37 thermal step-down) or into severe PROCHOT limp mode (<= 38% of base).
          const bool droppedBelowOsCap =
              (healthyOsLimitMhz > 0 && sampleMinLimitMhz < (healthyOsLimitMhz * 90) / 100);
          const bool severeProchot =
              (sampleMinLimitMhz <= (sampleNominalMhz * 38) / 100);
          firmwareClamped = droppedBelowOsCap || severeProchot;
        }
      }

      const bool passiveClamped = (samplePassivePct > 0 && samplePassivePct < 98);
      const float dspLoad = audioCpuPct.load(std::memory_order_relaxed);
      const bool clockCrushedUnderLoad =
          (dspLoad >= 35.0f && sampleNominalMhz > 0 && sampleEffectiveMhz > 0 &&
           sampleEffectiveMhz < (sampleNominalMhz * 80) / 100);

      const bool rawThrottled =
          firmwareClamped || passiveClamped || wasOverTemp || clockCrushedUnderLoad;
      activelyThrottledFlag.store(rawThrottled, std::memory_order_relaxed);

      const juce::int64 nowMs = (juce::int64)juce::Time::getMillisecondCounter();
      if (rawThrottled) {
        latchedUntilMs.store(nowMs + kLatchDurationMs, std::memory_order_relaxed);

        juce::StringArray reasons;
        if (firmwareClamped) {
          reasons.add("Firmware clock clamp (" + juce::String(sampleMinLimitMhz) +
                      "/" + juce::String(sampleNominalMhz) + " MHz)");
        }
        if (passiveClamped) {
          reasons.add("ACPI passive thermal limit (" +
                      juce::String(samplePassivePct) + "%)");
        }
        if (wasOverTemp) {
          reasons.add("High CPU temperature (" + juce::String(sampleTempC) + " C)");
        }
        if (clockCrushedUnderLoad) {
          reasons.add("Clock crushed to ~" + juce::String(sampleEffectiveMhz) +
                      " MHz under " + juce::String(dspLoad, 0) + "% DSP load");
        }
        const juce::String joined = reasons.joinIntoString("; ");
        {
          const juce::ScopedLock sl(reasonLock);
          lastReason = joined;
        }
        if (!wasLoggedThrottle) {
          wasLoggedThrottle = true;
          LOG_WARN("CPU THERMAL / FIRMWARE THROTTLE DETECTED: " + joined +
                   (sampleTempC > 0 ? (" | Temp: " + juce::String(sampleTempC) + " C")
                                    : juce::String()));
        }
      } else if (wasLoggedThrottle && nowMs >= latchedUntilMs.load(std::memory_order_relaxed)) {
        wasLoggedThrottle = false;
        LOG_INFO("CPU thermal / firmware throttle cleared" +
                 (sampleTempC > 0 ? (" (Temp: " + juce::String(sampleTempC) + " C)")
                                  : juce::String()));
      }

      const bool showWarning =
          rawThrottled || (nowMs < latchedUntilMs.load(std::memory_order_relaxed));
      warningActiveFlag.store(showWarning, std::memory_order_relaxed);

      wait(2000);
    }

    if (query != nullptr) {
      ::PdhCloseQuery(query);
    }
#else
    while (!threadShouldExit()) {
      wait(2000);
    }
#endif
  }

#ifdef _WIN32
  static void readActiveOsFrequencyCap(int &outCapMhz, int &outThrottleMaxPct) {
    outCapMhz = 0;
    outThrottleMaxPct = 100;

    GUID *activeScheme = nullptr;
    if (::PowerGetActiveScheme(nullptr, &activeScheme) != ERROR_SUCCESS ||
        activeScheme == nullptr) {
      return;
    }

    // Subgroup: SUB_PROCESSOR {54533251-82be-4824-96c1-47b60b740d00}
    static const GUID kSubProcessor = {
        0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
    // PROCFREQMAX {75b0ae3f-bce0-45a7-8c89-c9611c25e100}
    static const GUID kProcFreqMax0 = {
        0x75b0ae3f, 0xbce0, 0x45a7, {0x8c, 0x89, 0xc9, 0x61, 0x1c, 0x25, 0xe1, 0x00}};
    // PROCFREQMAX1 {75b0ae3f-bce0-45a7-8c89-c9611c25e101}
    static const GUID kProcFreqMax1 = {
        0x75b0ae3f, 0xbce0, 0x45a7, {0x8c, 0x89, 0xc9, 0x61, 0x1c, 0x25, 0xe1, 0x01}};
    // PROCTHROTTLEMAX {bc5038f7-23e0-4960-96da-33abaf5935ec}
    static const GUID kProcThrottleMax = {
        0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}};

    SYSTEM_POWER_STATUS sps{};
    const bool onBattery =
        (::GetSystemPowerStatus(&sps) != 0 && sps.ACLineStatus == 0);

    auto readVal = [&](const GUID &settingGuid, DWORD &outVal) -> bool {
      return (onBattery ? ::PowerReadDCValueIndex(nullptr, activeScheme, &kSubProcessor,
                                                  &settingGuid, &outVal)
                        : ::PowerReadACValueIndex(nullptr, activeScheme, &kSubProcessor,
                                                  &settingGuid, &outVal)) == ERROR_SUCCESS;
    };

    DWORD f0 = 0, f1 = 0, tmax = 100;
    if (readVal(kProcFreqMax0, f0) && f0 > 0)
      outCapMhz = (int)f0;
    if (readVal(kProcFreqMax1, f1) && f1 > 0) {
      if (outCapMhz == 0 || (int)f1 < outCapMhz)
        outCapMhz = (int)f1;
    }
    if (readVal(kProcThrottleMax, tmax) && tmax > 0 && tmax <= 100)
      outThrottleMaxPct = (int)tmax;

    ::LocalFree(activeScheme);
  }

  static double readMaxCounterArrayValue(PDH_HCOUNTER counter) {
    DWORD bufSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS st = ::PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                                                   &bufSize, &itemCount, nullptr);
    if (st != (PDH_STATUS)PDH_MORE_DATA || bufSize == 0 || itemCount == 0)
      return -1.0;

    std::vector<uint8_t> raw(bufSize);
    auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(raw.data());
    if (::PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufSize,
                                       &itemCount, items) != ERROR_SUCCESS)
      return -1.0;

    double best = -1.0;
    for (DWORD i = 0; i < itemCount; ++i) {
      if (items[i].FmtValue.CStatus == ERROR_SUCCESS &&
          items[i].FmtValue.doubleValue > best) {
        best = items[i].FmtValue.doubleValue;
      }
    }
    return best;
  }

  static double readMinCounterArrayValue(PDH_HCOUNTER counter) {
    DWORD bufSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS st = ::PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                                                   &bufSize, &itemCount, nullptr);
    if (st != (PDH_STATUS)PDH_MORE_DATA || bufSize == 0 || itemCount == 0)
      return -1.0;

    std::vector<uint8_t> raw(bufSize);
    auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(raw.data());
    if (::PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufSize,
                                       &itemCount, items) != ERROR_SUCCESS)
      return -1.0;

    double best = -1.0;
    for (DWORD i = 0; i < itemCount; ++i) {
      if (items[i].FmtValue.CStatus == ERROR_SUCCESS &&
          items[i].FmtValue.doubleValue > 0.0) {
        if (best < 0.0 || items[i].FmtValue.doubleValue < best)
          best = items[i].FmtValue.doubleValue;
      }
    }
    return best;
  }
#endif
};

