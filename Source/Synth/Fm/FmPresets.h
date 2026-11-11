#pragma once

// Curated vintage FM factory preset catalog (DX7, DX100, TX81Z) with musical
// category organization, hardware provenance metadata, and voiced stereo FX.

#include <cstdint>
#include "FmVoiceData.h"

namespace FanfareFm {

inline constexpr int kNumPresetFxParams = 12;

struct FmFactoryPreset {
  const char *name;
  const char *category;
  const char *provenance;
  uint8_t patchBytes[kPatchSize];
  float fx[kNumPresetFxParams];
};

const FmFactoryPreset *factoryFmPresets(int &count) noexcept;
const char *const *fmPresetCategoryOrder(int &count) noexcept;

} // namespace FanfareFm
