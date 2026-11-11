// Fanfare-owned translation unit for the vendored msfa core and curated
// built-in vintage FM preset library.

#include "FmPresets.h"

void dexed_trace(const char *, const char *, ...) {}

namespace FanfareFm {

#include "FmPresetLibrary.inc"

const FmFactoryPreset *factoryFmPresets(int &count) noexcept {
  count = static_cast<int>(sizeof(kBuiltInFmLibrary) /
                           sizeof(kBuiltInFmLibrary[0]));
  return kBuiltInFmLibrary;
}

const char *const *fmPresetCategoryOrder(int &count) noexcept {
  static const char *const kOrder[] = {
      "Piano & Keys",          "Electric Piano & Clav", "Organ",
      "Bass",                  "Pads & Strings",        "Brass & Winds",
      "Leads & Synth",         "Bells & Mallets",       "Plucked & Guitar",
      "Drums & Percussion",    "SFX & Atmospheres",
  };
  count = static_cast<int>(sizeof(kOrder) / sizeof(kOrder[0]));
  return kOrder;
}

} // namespace FanfareFm
