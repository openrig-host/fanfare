#pragma once

#include <JuceHeader.h>

// Per-build bisect overrides (generated before each test build; optional).
#if __has_include("FanfareBisectFlags.h")
#include "FanfareBisectFlags.h"
#endif

// ==============================================================================
// Fanfare Constants
// Central location for all magic numbers and configuration values
// ==============================================================================

namespace FanfareConstants {

// ===== BISECT FLAGS (Oct 12, T14 Gen 3 storm investigation) =====
#ifndef FANFARE_PRIORITY_ELEVATION
#define FANFARE_PRIORITY_ELEVATION 1
#endif
#ifndef FANFARE_WORKING_SET_LOCK
#define FANFARE_WORKING_SET_LOCK 1
#endif
#ifndef FANFARE_CPU_SHELTER
#define FANFARE_CPU_SHELTER 1
#endif
#ifndef FANFARE_MMCSS
#define FANFARE_MMCSS 0
#endif
#ifndef FANFARE_EXEC_STATE_LOCK
#define FANFARE_EXEC_STATE_LOCK 1
#endif
#ifndef FANFARE_POWER_SCHEME_MGMT
#define FANFARE_POWER_SCHEME_MGMT 0
#endif
inline constexpr bool kPriorityElevationEnabled = FANFARE_PRIORITY_ELEVATION != 0;
inline constexpr bool kWorkingSetLockEnabled = FANFARE_WORKING_SET_LOCK != 0;
inline constexpr bool kCpuShelterEnabled = FANFARE_CPU_SHELTER != 0;
inline constexpr bool kMmcssEnabled = FANFARE_MMCSS != 0;
inline constexpr bool kExecStateLockEnabled = FANFARE_EXEC_STATE_LOCK != 0;
inline constexpr bool kPowerSchemeMgmtEnabled = FANFARE_POWER_SCHEME_MGMT != 0;

// --- Slot Configuration ---
constexpr int kNumSlots = 24;
constexpr int kNumFxSlotsPerBus = 3;
constexpr int kNumSetupButtons = 10;

// --- Audio Configuration ---
constexpr int kMaxInternalChannels = 32; // Internal buffer size for multi-out VST3s
constexpr int kDefaultInputChannels = 18; // Tascam US-1800 / Behringer UMC1820
constexpr int kDefaultOutputChannels = 8;
constexpr int kDefaultSampleRate = 44100;
constexpr int kDefaultBufferSize = 512;

// --- Default Hardware Input Mappings (Tascam US-1800) ---
constexpr int kMonitorInputChannel = 0;    // Hardware In 1
constexpr int kKeyboardInputChannel = 10;   // Hardware In 11 (previously RD88, now CK88)
constexpr int kAccordionInputChannel = 12; // Hardware In 13

// --- Output Bus Routing ---
constexpr int kDefaultFohOutputOffset = 0; // Channels 1+2
constexpr int kDefaultIemOutputOffset = 2; // Channels 3+4

// --- UI Configuration ---
constexpr int kDefaultWindowWidth = 1240;
constexpr int kDefaultWindowHeight = 760;
constexpr int kTimerIntervalMs = 50;
constexpr int kMeterSegments = 16;

// --- MIDI Configuration ---
constexpr int kDefaultMidiChannel = 1;
constexpr int kSustainPedalCC = 64;
constexpr int kMidiMaxVelocity = 127;
constexpr int kMidiMaxCCValue = 127;

// --- Arpeggiator Defaults ---
constexpr float kDefaultArpBpm = 120.0f;
constexpr int kDefaultArpOctavesUp = 1;
constexpr int kDefaultArpOctavesDown = 0;
constexpr float kDefaultArpGate = 0.9f;
constexpr int kDefaultArpPatternIdx = 0; // SimpleArpeggiator::Pattern::Up

// --- Reliability Configuration ---
constexpr int kMaxPluginExceptionsBeforeBypass = 5;

// --- Audio Callback Timing Telemetry ---
constexpr double kLateCallbackRatio = 1.5;      // audibly late, below the XRUN threshold
constexpr double kUnderrunCallbackRatio = 2.2;  // counted as an XRUN on the status bar
constexpr double kHeavyCallbackLoad = 0.7;      // callback used >70% of its block: DSP-bound
constexpr int kJitterReportIntervalMs = 30000;
constexpr int kMidiScratchBytes = 4096;
constexpr int kMidiHotplugProbeWindowMs = 8000; // keep polling WinMM briefly after a USB event

// --- CK88 (Roland) Default CCs ---
constexpr int kCk88SlotIndex = 1;
constexpr int kCk88ModWheelCC = 9;     // CK88 mod wheel (non-standard)
constexpr int kCk88SustainCC = 64;     // Sustain pedal
constexpr int kCk88ChorusCC = 93;      // Chorus / delay knob 1
constexpr int kCk88ReverbCC = 92;      // Reverb / delay knob 2
constexpr int kCk88Drawbar16CC = 77;   // 16' drawbar
constexpr int kCk88Drawbar513CC = 78;  // 5-1/3' drawbar
constexpr int kCk88Drawbar8CC = 79;    // 8' drawbar
constexpr int kCk88Drawbar4CC = 80;    // 4' drawbar
constexpr int kCk88Drawbar223CC = 81;  // 2-2/3' drawbar
constexpr int kCk88Drawbar2CC = 82;    // 2' drawbar
constexpr int kCk88Drawbar135CC = 83;  // 1-3/5' drawbar
constexpr int kCk88Drawbar113CC = 85;  // 1-1/3' drawbar
constexpr int kCk88Drawbar1CC = 86;    // 1' drawbar

// --- Consolidated Paths ---
inline juce::File getAppDirectory() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Fanfare");
}
inline juce::File getVst3Directory() {
#if JUCE_MAC
    return juce::File("/Library/Audio/Plug-Ins/VST3");
#elif JUCE_LINUX
    return juce::File("/usr/lib/vst3");
#else
    juce::File progFiles = juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory);
    juce::File vst3Dir = progFiles.getChildFile("Common Files").getChildFile("VST3");
    if (vst3Dir.exists() && vst3Dir.isDirectory())
        return vst3Dir;
    return juce::File("C:\\Program Files\\Common Files\\VST3");
#endif
}
inline juce::Array<juce::File> getVst3SearchDirectories() {
    juce::Array<juce::File> dirs;
#if JUCE_MAC
    dirs.add(juce::File("/Library/Audio/Plug-Ins/VST3"));
    dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                 .getChildFile("Library/Audio/Plug-Ins/VST3"));
#else
    dirs.add(getVst3Directory());
#endif
    return dirs;
}
inline bool isAudioUnitIdentifier(const juce::String& path) {
    return path.trim().startsWithIgnoreCase("AudioUnit:");
}
inline bool pluginExistsOnDisk(const juce::String& path) {
    const juce::String trimmed = path.trim();
    if (trimmed.isEmpty())
        return false;
    if (isAudioUnitIdentifier(trimmed))
        return true;
    return juce::File(trimmed).exists();
}
inline juce::File getSongsDirectory() {
    return getAppDirectory().getChildFile("songs");
}
inline juce::File getSetsDirectory() {
    return getAppDirectory().getChildFile("sets");
}
inline juce::File getBackupsDirectory() {
    return getAppDirectory().getChildFile("backups");
}
inline juce::File getSettingsDirectory() {
    return getAppDirectory().getChildFile("settings");
}

} // namespace FanfareConstants
