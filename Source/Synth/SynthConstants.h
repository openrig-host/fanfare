#pragma once

#include <cstdint>

namespace FanfareSynth {

namespace StateSchema {
inline constexpr int kCurrentVersion = 2;
inline constexpr const char *kKeyVersion = "version";
inline constexpr const char *kKeyEnabled = "enabled";
inline constexpr const char *kKeyPresetName = "presetName";
inline constexpr const char *kKeyPresetDirty = "presetDirty";
inline constexpr const char *kKeyCcMap = "ccMap";
inline constexpr const char *kDefaultInitPresetName = "Init Patch";
inline constexpr const char *kDefaultSlotPatchName = "Custom Patch";
inline constexpr const char *kUserPresetsSubdir = "synth-presets";
} // namespace StateSchema

namespace DspConfig {
inline constexpr double kMinSampleRate = 8000.0;
inline constexpr double kDefaultSampleRate = 48000.0;
inline constexpr int kMinBlockSize = 256;
inline constexpr int kControlRateStep = 16;

inline constexpr int kMaxPlayableVoices = 16;
inline constexpr int kStealTailSlots = 6;
inline constexpr double kStealFadeSeconds = 0.0035;
inline constexpr int kMaxMonoStackDepth = 16;

inline constexpr float kVoiceHeadroom = 0.21f;
inline constexpr double kMinNoteVelocity = 0.04;
inline constexpr double kReferenceMidiNote = 69.0;
inline constexpr double kKeyTrackPivotNote = 60.0;
inline constexpr double kReferencePitchHz = 440.0;
inline constexpr double kSemitonesPerOctave = 12.0;
inline constexpr double kCentsPerOctave = 1200.0;

inline constexpr double kMinOscFreqHz = 2.0;
inline constexpr double kMaxNyquistRatio = 0.45;
inline constexpr double kMinPulseWidth = 0.05;
inline constexpr double kMaxPulseWidth = 0.95;
inline constexpr double kPwmCenterMin = 0.10;
inline constexpr double kPwmCenterMax = 0.90;
inline constexpr double kPwmMaxModSpan = 0.42;

inline constexpr double kMinCutoffHz = 18.0;
inline constexpr double kMaxCutoffHz = 20000.0;
inline constexpr double kFilterEnvMaxOctaves = 5.5;
inline constexpr double kLfoFilterMaxOctaves = 2.5;
inline constexpr double kModWheelVibratoCents = 45.0;
inline constexpr double kModWheelFilterOctaves = 0.35;
inline constexpr double kAftertouchVibratoCents = 25.0;
inline constexpr double kAftertouchFilterOctaves = 0.85;

inline constexpr double kLadderMaxResonanceK = 4.22;
inline constexpr double kLadderBassCompAmount = 0.45;
inline constexpr double kHpfBoostShelfEnd = 0.12;
inline constexpr double kHpfBoostMaxDb = 4.0;
inline constexpr double kHpfSweepMinHz = 22.0;
inline constexpr double kHpfSweepMaxHz = 850.0;

inline constexpr double kAttackTargetOvershoot = 1.28;
inline constexpr double kDecayTimeConstantTau = 3.0;
inline constexpr double kEnvSilenceFloor = 0.00025;
inline constexpr double kGateSlewedSeconds = 0.0025;
inline constexpr double kLfoFadeRampSeconds = 0.85;
inline constexpr double kGlideTauScale = 0.22;

inline constexpr double kParamSmoothFastSec = 0.006;
inline constexpr double kParamSmoothMediumSec = 0.015;

inline constexpr double kVintageMaxPitchCents = 3.5;
inline constexpr double kVintageMaxCutoffOctaves = 0.08;
inline constexpr double kVintageMaxEnvScale = 0.07;
inline constexpr double kVintageWanderHz = 0.18;

inline constexpr double kChorusBufferSeconds = 0.065;
inline constexpr double kChorusBaseDelaySec = 0.0055;
inline constexpr float kChorusModeIRateHz = 0.51f;
inline constexpr float kChorusModeIDepthMs = 2.3f;
inline constexpr float kChorusModeIIRateHz = 0.82f;
inline constexpr float kChorusModeIIDepthMs = 3.6f;
inline constexpr float kChorusModeBothRateHz = 4.2f;
inline constexpr float kChorusModeBothDepthMs = 0.75f;
inline constexpr float kChorusCustomMinDepthMs = 0.6f;
inline constexpr float kChorusCustomSpanDepthMs = 5.4f;
inline constexpr double kChorusBbdLowpassHz = 9200.0;

inline constexpr double kDelayMaxBufferSeconds = 1.65;
inline constexpr double kDelayTimeSmoothSec = 0.045;
inline constexpr double kDelayHighpassHz = 85.0;
inline constexpr double kDelayDampBrightHz = 12000.0;
inline constexpr double kDelayDampDarkHz = 1400.0;

inline constexpr int kMidiSustainPedalCC = 64;
inline constexpr int kMidiModWheelCC = 1;
inline constexpr int kMidiAllSoundOffCC = 120;
inline constexpr int kMidiAllNotesOffCC = 123;
inline constexpr int kUnassignedCC = -1;
} // namespace DspConfig

namespace UiMetrics {
inline constexpr int kDefaultOverlayWidth = 1180;
inline constexpr int kDefaultOverlayHeight = 720;
inline constexpr int kMinOverlayWidth = 980;
inline constexpr int kMinOverlayHeight = 610;
inline constexpr int kMaxOverlayWidth = 1580;
inline constexpr int kMaxOverlayHeight = 940;
inline constexpr int kOverlayMarginFromHost = 36;

inline constexpr int kOuterPadding = 8;
inline constexpr int kHeaderHeight = 42;
inline constexpr int kSectionGap = 6;
inline constexpr int kControlCheekWidth = 164;
inline constexpr int kSectionHeaderHeight = 19;
inline constexpr int kKeyboardStripHeight = 108;
inline constexpr int kWheelStripWidth = 34;

inline constexpr int kControlLabelHeight = 14;
inline constexpr int kValueReadoutHeight = 16;
inline constexpr int kFaderMinVirtualSpanPx = 165;
inline constexpr float kKnobVirtualSpanPx = 175.0f;
inline constexpr float kFineTuneGearDivisor = 10.0f;
inline constexpr float kWheelFineMultiplier = 0.2f;
inline constexpr float kBipolarDetentRadius = 0.022f;
inline constexpr float kDisabledControlAlpha = 0.36f;
inline constexpr int kTelemetryTimerHz = 24;
} // namespace UiMetrics

} // namespace FanfareSynth
