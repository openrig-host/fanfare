#pragma once

#include <JuceHeader.h>

namespace FanfareSynth::Ui {

class SynthTelemetrySink {
public:
  virtual ~SynthTelemetrySink() = default;
  virtual void handleParamHovered(int paramIdx) = 0;
  virtual void handleParamTouchState(int paramIdx, bool isDragging,
                                     bool fineMode) = 0;
  virtual void handleParamEdited(int paramIdx) = 0;
  virtual void showParamContextMenu(int paramIdx,
                                    juce::Component *anchor) = 0;
};

class SynthParamWidget : public juce::Component,
                         public juce::SettableTooltipClient {
public:
  explicit SynthParamWidget(int parameterIndex) : paramIdx(parameterIndex) {}
  ~SynthParamWidget() override = default;

  int getParamIndex() const noexcept { return paramIdx; }
  virtual void syncFromProcessor() = 0;
  virtual void setControlDimmed(bool dimmed) = 0;
  virtual void beginInlineValueEdit() {}

protected:
  int paramIdx = 0;
};

} // namespace FanfareSynth::Ui
