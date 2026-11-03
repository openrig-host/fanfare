#pragma once

#include "ChannelStripProcessor.h"
#include "RackSlot.h"
#include <JuceHeader.h>
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "HelpLauncher.h"
#include "ThemeManager.h"

class BigKnob : public juce::Slider {
public:
  BigKnob(const juce::String &suffix) {
    setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 20);
    setTextValueSuffix(suffix);
  }
};

class GainReductionMeter : public juce::Component, public juce::Timer {
public:
  GainReductionMeter(FanfareDSP::SimpleComp &c) : comp(c) { startTimer(30); }

  void timerCallback() override {
    float targetGr = comp.getGainReductionDb();
    float previousGr = lastGr;

    if (targetGr > lastGr) {
      lastGr = targetGr;
    } else {
      lastGr += (targetGr - lastGr) * 0.20f;
      if (std::abs(lastGr - targetGr) < 0.05f)
        lastGr = targetGr;
    }

    if (std::abs(lastGr - previousGr) > 0.01f)
      repaint();
  }

  void paint(juce::Graphics &g) override {
    auto bounds = getLocalBounds().toFloat();

    // Recessed hardware meter well
    Fanfare::ModalFrame::paintRecessedWell(g, bounds, 3.0f);

    float height = bounds.getHeight();
    // Scale up to 24 dB of gain reduction
    float normalizedGr = juce::jlimit(0.0f, 1.0f, lastGr / 24.0f);
    float barHeight = normalizedGr * (height - 4.0f);

    if (barHeight > 1.0f) {
      auto barBounds = bounds.reduced(2.0f);
      auto grRect = barBounds.removeFromTop(barHeight);

      juce::ColourGradient grad(
          ThemeManager::get(Theme::Role::warn), grRect.getX(), grRect.getY(),
          ThemeManager::get(Theme::Role::meterPeak), grRect.getX(), grRect.getBottom(), false);
      g.setGradientFill(grad);
      g.fillRoundedRectangle(grRect, 1.5f);
    }

    // Scale tick marks (-6dB, -12dB, -18dB)
    g.setColour(ThemeManager::get(Theme::Role::engrave).withAlpha(0.65f));
    float y6 = bounds.getY() + (6.0f / 24.0f) * height;
    float y12 = bounds.getY() + (12.0f / 24.0f) * height;
    float y18 = bounds.getY() + (18.0f / 24.0f) * height;
    g.fillRect(bounds.getX() + 2.0f, y6, 4.0f, 1.0f);
    g.fillRect(bounds.getX() + 2.0f, y12, 4.0f, 1.0f);
    g.fillRect(bounds.getX() + 2.0f, y18, 4.0f, 1.0f);
  }

private:
  FanfareDSP::SimpleComp &comp;
  float lastGr = 0.0f;
};

class ChannelStripComponent : public juce::Component,
                              public juce::ChangeListener {
public:
  ChannelStripComponent(RackSlot &s, std::function<void()> onClose)
      : slot(s), onCloseRequest(onClose),
        grMeter(slot.getStrip().getCompReference()) {

    auto styleGroup = [](juce::GroupComponent &grp, const juce::String &title) {
      grp.setText(title);
      grp.setColour(juce::GroupComponent::outlineColourId, ThemeManager::get(Theme::Role::hairlineStrong));
      grp.setColour(juce::GroupComponent::textColourId, ThemeManager::get(Theme::Role::engrave));
    };

    // Close & Help Buttons
    addAndMakeVisible(closeBtn);
    Fanfare::styleHelpButton(helpBtn, "#channel-strip-dsp", "HAAALP! Open the Strip DSP section of the User's Manual");
    addAndMakeVisible(helpBtn);
    closeBtn.setButtonText("CLOSE");
    Fanfare::setButtonRole(closeBtn, Fanfare::ButtonRole::Danger);
    closeBtn.onClick = [this] {
      if (onCloseRequest)
        onCloseRequest();
    };

    // Stereo / Mono Mode Button (cycles STEREO -> MONO (L+R) -> MONO (Left) -> MONO (Right))
    addAndMakeVisible(stereoMonoBtn);
    updateStereoMonoButton();
    stereoMonoBtn.setTooltip("Input Channel Mode: Click to cycle STEREO -> MONO (L+R) -> MONO (Left) -> MONO (Right)");
    stereoMonoBtn.onClick = [this] {
      int next = (slot.getStrip().channelMode.load() + 1) % 4;
      slot.getStrip().channelMode.store(next);
      updateStereoMonoButton();
    };

    // --- GATE ---
    addAndMakeVisible(gateGroup);
    styleGroup(gateGroup, "NOISE GATE");

    addAndMakeVisible(gateToggle);
    gateToggle.setButtonText("ENABLE");
    gateToggle.setClickingTogglesState(true);
    gateToggle.getProperties().set("useToggleSwitch", true);
    gateToggle.getProperties().set("isOrangeToggle", false);
    gateToggle.setToggleState(slot.getStrip().gateEnabled, juce::dontSendNotification);
    gateToggle.onClick = [this] {
      slot.getStrip().gateEnabled = gateToggle.getToggleState();
    };

    addAndMakeVisible(gateThreshKnob);
    gateThreshKnob.setRange(-80.0, 0.0, 1.0);
    gateThreshKnob.setValue(slot.getStrip().gateThreshold, juce::dontSendNotification);
    gateThreshKnob.onValueChange = [this] {
      slot.getStrip().gateThreshold = (float)gateThreshKnob.getValue();
    };

    // --- 10-BAND GRAPHIC EQ ---
    addAndMakeVisible(eqGroup);
    styleGroup(eqGroup, "10-BAND GRAPHIC EQ");

    addAndMakeVisible(eqToggle);
    eqToggle.setButtonText("ENABLE");
    eqToggle.setClickingTogglesState(true);
    eqToggle.getProperties().set("useToggleSwitch", true);
    eqToggle.getProperties().set("isOrangeToggle", false);
    eqToggle.setToggleState(slot.getStrip().eqEnabled, juce::dontSendNotification);
    eqToggle.onClick = [this] {
      slot.getStrip().eqEnabled = eqToggle.getToggleState();
    };

    addAndMakeVisible(flatBtn);
    flatBtn.setButtonText("FLAT");
    flatBtn.setTooltip("Reset all 10 EQ bands to 0 dB");
    Fanfare::setButtonRole(flatBtn, Fanfare::ButtonRole::Standard);
    flatBtn.onClick = [this] {
      activeDragBand = -1;
      for (int i = 0; i < 10; ++i) {
        slot.getStrip().eqBands[i].store(0.0f);
        lastEqBandValues[i] = 0.0;
        dragAnchorBands[i] = 0.0;
        if (eqSliders[i]) eqSliders[i]->setValue(0.0, juce::dontSendNotification);
      }
    };

    addAndMakeVisible(eqCurveModeBtn);
    updateEqCurveModeButton();
    eqCurveModeBtn.onClick = [this] {
      const bool next = !slot.getStrip().eqCurveMode.load();
      slot.getStrip().eqCurveMode.store(next);
      activeDragBand = -1;
      for (int i = 0; i < 10; ++i) {
        const double v = eqSliders[i] ? eqSliders[i]->getValue() : (double)slot.getStrip().eqBands[i].load();
        dragAnchorBands[i] = v;
        lastEqBandValues[i] = v;
      }
      updateEqCurveModeButton();
    };

    // Vol Slider (Input Trim)
    addAndMakeVisible(eqVolSlider);
    eqVolSlider.setSliderStyle(juce::Slider::LinearVertical);
    eqVolSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    eqVolSlider.setRange(-12.0, 12.0, 0.1);
    eqVolSlider.setValue(slot.getStrip().eqVolGain.load(), juce::dontSendNotification);
    eqVolSlider.setDoubleClickReturnValue(true, 0.0);
    eqVolSlider.setTooltip("Input Trim Gain (-12 dB to +12 dB)");
    eqVolSlider.onValueChange = [this] {
      slot.getStrip().eqVolGain.store((float)eqVolSlider.getValue());
    };

    addAndMakeVisible(eqVolLabel);
    eqVolLabel.setText("VOL", juce::dontSendNotification);
    eqVolLabel.setFont(juce::FontOptions("Barlow Condensed", 11.5f, juce::Font::bold));
    eqVolLabel.setJustificationType(juce::Justification::centred);
    eqVolLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::engrave));

    // 10 Frequency Band Sliders
    const char* bandNames[10] = { "31.25", "62.5", "125", "250", "500", "1K", "2K", "4K", "8K", "16K" };
    for (int i = 0; i < 10; ++i) {
      eqSliders[i] = std::make_unique<juce::Slider>();
      addAndMakeVisible(*eqSliders[i]);
      eqSliders[i]->setSliderStyle(juce::Slider::LinearVertical);
      eqSliders[i]->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
      eqSliders[i]->setRange(-12.0, 12.0, 0.1);
      const float initialBandDb = slot.getStrip().eqBands[i].load();
      eqSliders[i]->setValue(initialBandDb, juce::dontSendNotification);
      lastEqBandValues[i] = initialBandDb;
      dragAnchorBands[i] = initialBandDb;
      eqSliders[i]->setDoubleClickReturnValue(true, 0.0);
      eqSliders[i]->setTooltip(juce::String(bandNames[i]) + " Hz Band (-12 dB to +12 dB; hold Alt to bypass Curve Link)");
      int bandIdx = i;
      eqSliders[i]->onDragStart = [this, bandIdx] {
        activeDragBand = bandIdx;
        for (int b = 0; b < 10; ++b)
          dragAnchorBands[b] = eqSliders[b] ? eqSliders[b]->getValue() : (double)slot.getStrip().eqBands[b].load();
      };
      eqSliders[i]->onDragEnd = [this, bandIdx] {
        if (activeDragBand == bandIdx)
          activeDragBand = -1;
        for (int b = 0; b < 10; ++b)
          lastEqBandValues[b] = eqSliders[b] ? eqSliders[b]->getValue() : (double)slot.getStrip().eqBands[b].load();
      };
      eqSliders[i]->onValueChange = [this, bandIdx] {
        if (isUpdatingEqCurve)
          return;
        const double newVal = eqSliders[bandIdx]->getValue();
        slot.getStrip().eqBands[bandIdx].store((float)newVal);

        const bool curveActive = slot.getStrip().eqCurveMode.load() &&
                                 !juce::ModifierKeys::currentModifiers.isAltDown();
        if (curveActive) {
          static constexpr double kCurveWeights[4] = { 1.0, 0.62, 0.28, 0.08 };
          isUpdatingEqCurve = true;
          if (activeDragBand == bandIdx) {
            const double delta = newVal - dragAnchorBands[bandIdx];
            for (int b = 0; b < 10; ++b) {
              if (b == bandIdx)
                continue;
              const int dist = std::abs(b - bandIdx);
              if (dist <= 3 && eqSliders[b]) {
                const double coupled = juce::jlimit(-12.0, 12.0, dragAnchorBands[b] + delta * kCurveWeights[dist]);
                eqSliders[b]->setValue(coupled, juce::dontSendNotification);
                const double snapped = eqSliders[b]->getValue();
                slot.getStrip().eqBands[b].store((float)snapped);
                lastEqBandValues[b] = snapped;
              }
            }
          } else {
            // Non-drag change (mouse wheel or double-click reset): apply proportional delta
            const double delta = newVal - lastEqBandValues[bandIdx];
            for (int b = 0; b < 10; ++b) {
              if (b == bandIdx)
                continue;
              const int dist = std::abs(b - bandIdx);
              if (dist <= 3 && eqSliders[b]) {
                const double coupled = juce::jlimit(-12.0, 12.0, eqSliders[b]->getValue() + delta * kCurveWeights[dist]);
                eqSliders[b]->setValue(coupled, juce::dontSendNotification);
                const double snapped = eqSliders[b]->getValue();
                slot.getStrip().eqBands[b].store((float)snapped);
                lastEqBandValues[b] = snapped;
              }
            }
          }
          isUpdatingEqCurve = false;
        }
        lastEqBandValues[bandIdx] = newVal;
      };

      eqLabels[i] = std::make_unique<juce::Label>();
      addAndMakeVisible(*eqLabels[i]);
      eqLabels[i]->setText(bandNames[i], juce::dontSendNotification);
      eqLabels[i]->setFont(juce::FontOptions("Barlow Condensed", 11.0f, juce::Font::bold));
      eqLabels[i]->setJustificationType(juce::Justification::centred);
      eqLabels[i]->setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::engrave));
    }

    // Gain Slider (Master Trim)
    addAndMakeVisible(eqGainSlider);
    eqGainSlider.setSliderStyle(juce::Slider::LinearVertical);
    eqGainSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    eqGainSlider.setRange(-12.0, 12.0, 0.1);
    eqGainSlider.setValue(slot.getStrip().eqMasterGain.load(), juce::dontSendNotification);
    eqGainSlider.setDoubleClickReturnValue(true, 0.0);
    eqGainSlider.setTooltip("Master EQ Output Gain (-12 dB to +12 dB)");
    eqGainSlider.onValueChange = [this] {
      slot.getStrip().eqMasterGain.store((float)eqGainSlider.getValue());
    };

    addAndMakeVisible(eqGainLabel);
    eqGainLabel.setText("GAIN", juce::dontSendNotification);
    eqGainLabel.setFont(juce::FontOptions("Barlow Condensed", 11.5f, juce::Font::bold));
    eqGainLabel.setJustificationType(juce::Justification::centred);
    eqGainLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::engrave));

    // --- COMP ---
    addAndMakeVisible(compGroup);
    styleGroup(compGroup, "COMPRESSOR");

    addAndMakeVisible(compToggle);
    compToggle.setButtonText("ENABLE");
    compToggle.setClickingTogglesState(true);
    compToggle.getProperties().set("useToggleSwitch", true);
    compToggle.getProperties().set("isOrangeToggle", false);
    compToggle.setToggleState(slot.getStrip().compEnabled, juce::dontSendNotification);
    compToggle.onClick = [this] {
      slot.getStrip().compEnabled = compToggle.getToggleState();
    };

    addAndMakeVisible(compPresetCombo);
    compPresetCombo.addItem("Preset: Manual", 1);
    compPresetCombo.addItem("Preset: MP3 / Track Leveler", 2);
    compPresetCombo.addItem("Preset: Vocal / Lead Focus", 3);
    compPresetCombo.addItem("Preset: Punchy Drums", 4);
    compPresetCombo.addItem("Preset: Brickwall Limiter", 5);
    compPresetCombo.setSelectedId(slot.getStrip().compPreset.load() + 1, juce::dontSendNotification);
    compPresetCombo.onChange = [this] {
      int id = compPresetCombo.getSelectedId() - 1;
      slot.getStrip().compPreset.store(id);
    };

    addAndMakeVisible(compAmtKnob);
    compAmtKnob.setRange(0.0, 1.0, 0.01);
    compAmtKnob.setValue(slot.getStrip().compAmount, juce::dontSendNotification);
    compAmtKnob.setTooltip("Amount / Threshold");
    compAmtKnob.onValueChange = [this] {
      slot.getStrip().compAmount = (float)compAmtKnob.getValue();
    };

    addAndMakeVisible(compMakeupKnob);
    compMakeupKnob.setRange(-12.0, 18.0, 0.5);
    compMakeupKnob.setValue(slot.getStrip().compMakeupDb.load(), juce::dontSendNotification);
    compMakeupKnob.setTooltip("Makeup Gain (dB)");
    compMakeupKnob.onValueChange = [this] {
      slot.getStrip().compMakeupDb.store((float)compMakeupKnob.getValue());
    };

    addAndMakeVisible(grMeter);

    // --- REVERB & CHORUS ---
    addAndMakeVisible(revChoGroup);
    styleGroup(revChoGroup, "SPACE / MOD");

    // Reverb
    addAndMakeVisible(reverbToggle);
    reverbToggle.setButtonText("REVERB");
    reverbToggle.setClickingTogglesState(true);
    reverbToggle.getProperties().set("useToggleSwitch", true);
    reverbToggle.setToggleState(slot.getStrip().reverbEnabled, juce::dontSendNotification);
    reverbToggle.onClick = [this] {
      slot.getStrip().reverbEnabled = reverbToggle.getToggleState();
    };

    addAndMakeVisible(reverbSizeKnob);
    reverbSizeKnob.setRange(0.0, 1.0, 0.01);
    reverbSizeKnob.setValue(slot.getStrip().reverbSize, juce::dontSendNotification);
    reverbSizeKnob.setTooltip("Room Size");
    reverbSizeKnob.onValueChange = [this] {
      slot.getStrip().reverbSize = (float)reverbSizeKnob.getValue();
    };

    addAndMakeVisible(reverbMixKnob);
    reverbMixKnob.setRange(0.0, 1.0, 0.01);
    reverbMixKnob.setValue(slot.getStrip().reverbMix, juce::dontSendNotification);
    reverbMixKnob.setTooltip("Reverb Mix");
    reverbMixKnob.onValueChange = [this] {
      slot.getStrip().reverbMix = (float)reverbMixKnob.getValue();
    };

    // IR / convolution reverb
    addAndMakeVisible(irGroup);
    styleGroup(irGroup, "IMPULSE RESPONSE (CAB / ROOM)");

    addAndMakeVisible(irToggle);
    irToggle.setButtonText("ENABLE");
    irToggle.setClickingTogglesState(true);
    irToggle.getProperties().set("useToggleSwitch", true);
    irToggle.getProperties().set("isOrangeToggle", false);
    irToggle.setToggleState(slot.getStrip().irEnabled.load(), juce::dontSendNotification);
    irToggle.onClick = [this] {
      slot.getStrip().irEnabled.store(irToggle.getToggleState());
    };

    addAndMakeVisible(irLoadBtn);
    irLoadBtn.setButtonText("LOAD WAV");
    Fanfare::setButtonRole(irLoadBtn, Fanfare::ButtonRole::Standard);
    irLoadBtn.setTooltip("Load a custom impulse response (.wav) file from disk");
    irLoadBtn.onClick = [this] {
      irChooser = std::make_unique<juce::FileChooser>(
          "Load Impulse Response (.wav)",
          juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav");
      auto flags = juce::FileBrowserComponent::openMode |
                   juce::FileBrowserComponent::canSelectFiles;
      juce::Component::SafePointer<ChannelStripComponent> safeThis(this);
      irChooser->launchAsync(flags, [safeThis](const juce::FileChooser& fc) {
        if (safeThis == nullptr)
          return;
        auto f = fc.getResult();
        if (f == juce::File())
          return;
        if (safeThis->slot.getStrip().getIRReverb().loadIR(f)) {
          safeThis->irPresetCombo.setSelectedId(1, juce::dontSendNotification);
          safeThis->irNameLabel.setText(
              safeThis->slot.getStrip().getIRReverb().getDisplayName(),
              juce::dontSendNotification);
          safeThis->irToggle.setToggleState(true, juce::dontSendNotification);
          safeThis->slot.getStrip().irEnabled.store(true);
          if (safeThis->slot.getStrip().irMix.load() < 0.02f) {
            safeThis->slot.getStrip().irMix.store(0.30f);
            safeThis->irMixKnob.setValue(0.30, juce::dontSendNotification);
          }
        }
      });
    };

    addAndMakeVisible(irPresetCombo);
    irPresetCombo.addItem("Preset: Custom / WAV...", 1);
    {
      const auto *presets = FanfareDSP::ConvolutionReverb::getBuiltinPresets();
      for (int i = 0; i < FanfareDSP::ConvolutionReverb::kNumBuiltinPresets; ++i)
        irPresetCombo.addItem(presets[i].menuLabel, i + 2);
      const int activeIdx = FanfareDSP::ConvolutionReverb::findBuiltinPresetIndex(
          slot.getStrip().getIRReverb().getLoadedPath());
      irPresetCombo.setSelectedId(activeIdx >= 0 ? (activeIdx + 2) : 1,
                                  juce::dontSendNotification);
    }
    irPresetCombo.setTooltip(
        "Built-in synthesized acoustic IR spaces and speaker cab (Rooms/Halls: 20-35% mix, 2x12 Cab: 50-100% mix)");
    irPresetCombo.onChange = [this] {
      const int id = irPresetCombo.getSelectedId();
      if (id >= 2) {
        if (slot.getStrip().getIRReverb().loadPresetByIndex(id - 2)) {
          irNameLabel.setText(slot.getStrip().getIRReverb().getDisplayName(),
                              juce::dontSendNotification);
          irToggle.setToggleState(true, juce::dontSendNotification);
          slot.getStrip().irEnabled.store(true);
          if (slot.getStrip().irMix.load() < 0.02f) {
            slot.getStrip().irMix.store(0.30f);
            irMixKnob.setValue(0.30, juce::dontSendNotification);
          }
        }
      }
    };

    addAndMakeVisible(irMixLabel);
    irMixLabel.setText("MIX", juce::dontSendNotification);
    irMixLabel.setFont(BoutiqueLookAndFeel::condensedFont(11.0f, 0.05f));
    irMixLabel.setJustificationType(juce::Justification::centred);
    irMixLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::engrave));

    addAndMakeVisible(irMixKnob);
    irMixKnob.setRange(0.0, 1.0, 0.01);
    irMixKnob.setDoubleClickReturnValue(true, 0.30);
    irMixKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 18);
    irMixKnob.textFromValueFunction = [](double val) {
      return juce::String(juce::roundToInt(val * 100.0)) + "%";
    };
    irMixKnob.valueFromTextFunction = [](const juce::String &txt) {
      return juce::jlimit(0.0, 1.0, txt.retainCharacters("0123456789.").getDoubleValue() * 0.01);
    };
    irMixKnob.setValue(slot.getStrip().irMix.load(), juce::dontSendNotification);
    irMixKnob.setTooltip("IR Wet Mix");
    irMixKnob.onValueChange = [this] {
      slot.getStrip().irMix.store((float)irMixKnob.getValue());
    };

    addAndMakeVisible(irNameLabel);
    irNameLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::textDim));
    irNameLabel.setJustificationType(juce::Justification::centredLeft);
    irNameLabel.setFont(juce::FontOptions(11.5f));
    irNameLabel.setText(slot.getStrip().getIRReverb().getDisplayName(),
                        juce::dontSendNotification);

    // Chorus
    addAndMakeVisible(chorusToggle);
    chorusToggle.setButtonText("CHORUS");
    chorusToggle.setClickingTogglesState(true);
    chorusToggle.getProperties().set("useToggleSwitch", true);
    chorusToggle.setToggleState(slot.getStrip().chorusEnabled, juce::dontSendNotification);
    chorusToggle.onClick = [this] {
      slot.getStrip().chorusEnabled = chorusToggle.getToggleState();
    };

    addAndMakeVisible(chorusRateKnob);
    chorusRateKnob.setRange(0.1, 5.0, 0.05);
    chorusRateKnob.setValue(slot.getStrip().chorusRate, juce::dontSendNotification);
    chorusRateKnob.setTooltip("Chorus Rate");
    chorusRateKnob.onValueChange = [this] {
      slot.getStrip().chorusRate = (float)chorusRateKnob.getValue();
    };

    addAndMakeVisible(chorusMixKnob);
    chorusMixKnob.setRange(0.0, 1.0, 0.01);
    chorusMixKnob.setValue(slot.getStrip().chorusMix, juce::dontSendNotification);
    chorusMixKnob.setTooltip("Chorus Mix");
    chorusMixKnob.onValueChange = [this] {
      slot.getStrip().chorusMix = (float)chorusMixKnob.getValue();
    };

    // --- CUSTOMIZE ---
    addAndMakeVisible(nameLabel);
    nameLabel.setText("STRIP NAME", juce::dontSendNotification);
    nameLabel.setFont(juce::FontOptions("Barlow Condensed", 12.0f, juce::Font::bold));
    nameLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::engrave));
    addAndMakeVisible(nameEditor);
    nameEditor.setText(slot.getName());
    nameEditor.onTextChange = [this] {
      slot.setName(nameEditor.getText());
      repaint();
    };

    // Color picker
    addAndMakeVisible(colorLabel);
    colorLabel.setText("STRIP COLOR", juce::dontSendNotification);
    colorLabel.setFont(juce::FontOptions("Barlow Condensed", 12.0f, juce::Font::bold));
    colorLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::engrave));
    addAndMakeVisible(colorPreview);
    colorPreview.setColour(juce::Label::backgroundColourId, slot.getChannelColor());
    colorPreview.setColour(juce::Label::outlineColourId, ThemeManager::get(Theme::Role::borderStrong));
    addAndMakeVisible(colorButton);
    colorButton.setButtonText("CHANGE COLOR");
    Fanfare::setButtonRole(colorButton, Fanfare::ButtonRole::Standard);
    colorButton.onClick = [this] { showColorPicker(); };

    // --- IEM MONITOR TRIM & AUX SENDS ---
    addAndMakeVisible(auxGroup);
    styleGroup(auxGroup, "IEM & AUX SENDS");

    auto styleMiniHeader = [](juce::Label &lbl, const juce::String &txt, juce::Colour col) {
      lbl.setText(txt, juce::dontSendNotification);
      lbl.setFont(BoutiqueLookAndFeel::condensedFont(11.0f, 0.05f));
      lbl.setJustificationType(juce::Justification::centred);
      lbl.setColour(juce::Label::textColourId, col);
    };

    addAndMakeVisible(iemLevelLabel);
    styleMiniHeader(iemLevelLabel, "IEM TRIM", ThemeManager::get(Theme::Role::iem));

    addAndMakeVisible(iemLevelKnob);
    iemLevelKnob.setComponentID("iem");
    iemLevelKnob.setRange(0.0, 2.0, 0.01);
    iemLevelKnob.setDoubleClickReturnValue(true, 1.0);
    iemLevelKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 78, 18);
    iemLevelKnob.textFromValueFunction = [](double val) {
      if (val <= 0.005)
        return juce::String("OFF");
      if (std::abs(val - 1.0) <= 0.008)
        return juce::String("0.0 dB");
      const double db = juce::Decibels::gainToDecibels(val, -60.0);
      return (db > 0.0 ? "+" : "") + juce::String(db, 1) + " dB";
    };
    iemLevelKnob.valueFromTextFunction = [](const juce::String &txt) {
      auto t = txt.trim();
      if (t.equalsIgnoreCase("off") || t == "-inf")
        return 0.0;
      if (t.endsWithIgnoreCase("%"))
        return juce::jlimit(0.0, 2.0, t.dropLastCharacters(1).getDoubleValue() * 0.01);
      const double db = t.retainCharacters("-+0123456789.").getDoubleValue();
      return juce::jlimit(0.0, 2.0, (double)juce::Decibels::decibelsToGain((float)db, -60.0f));
    };
    iemLevelKnob.setValue(slot.getIemOffset(), juce::dontSendNotification);
    iemLevelKnob.setTooltip(
        "Per-Strip IEM Trim relative to FOH fader (-inf to +6.0 dB; double-click for 0.0 dB unity)");
    iemLevelKnob.onValueChange = [this] {
      slot.setIemOffset((float)iemLevelKnob.getValue());
    };

    addAndMakeVisible(aux1Label);
    styleMiniHeader(aux1Label, "AUX 1", ThemeManager::get(Theme::Role::engrave));

    addAndMakeVisible(aux1Knob);
    aux1Knob.setRange(0.0, 1.0, 0.01);
    aux1Knob.setDoubleClickReturnValue(true, 0.0);
    aux1Knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 18);
    aux1Knob.textFromValueFunction = [](double val) {
      return juce::String(juce::roundToInt(val * 100.0)) + "%";
    };
    aux1Knob.valueFromTextFunction = [](const juce::String &txt) {
      return juce::jlimit(0.0, 1.0, txt.retainCharacters("0123456789.").getDoubleValue() * 0.01);
    };
    aux1Knob.setValue(slot.getAux1Send(), juce::dontSendNotification);
    aux1Knob.setTooltip("Post-fader send level to Aux 1 return");
    aux1Knob.onValueChange = [this] {
      slot.setAux1Send((float)aux1Knob.getValue());
    };

    addAndMakeVisible(aux2Label);
    styleMiniHeader(aux2Label, "AUX 2", ThemeManager::get(Theme::Role::engrave));

    addAndMakeVisible(aux2Knob);
    aux2Knob.setRange(0.0, 1.0, 0.01);
    aux2Knob.setDoubleClickReturnValue(true, 0.0);
    aux2Knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 18);
    aux2Knob.textFromValueFunction = [](double val) {
      return juce::String(juce::roundToInt(val * 100.0)) + "%";
    };
    aux2Knob.valueFromTextFunction = [](const juce::String &txt) {
      return juce::jlimit(0.0, 1.0, txt.retainCharacters("0123456789.").getDoubleValue() * 0.01);
    };
    aux2Knob.setValue(slot.getAux2Send(), juce::dontSendNotification);
    aux2Knob.setTooltip("Post-fader send level to Aux 2 return");
    aux2Knob.onValueChange = [this] {
      slot.setAux2Send((float)aux2Knob.getValue());
    };

    // --- MUSETTE DETUNER ---
    addAndMakeVisible(musetteGroup);
    styleGroup(musetteGroup, "MUSETTE DETUNER");

    addAndMakeVisible(musetteToggle);
    musetteToggle.setButtonText("ENABLE");
    musetteToggle.setClickingTogglesState(true);
    musetteToggle.getProperties().set("useToggleSwitch", true);
    musetteToggle.getProperties().set("isOrangeToggle", false);
    musetteToggle.setToggleState(slot.getStrip().musetteEnabled.load(), juce::dontSendNotification);
    musetteToggle.setTooltip("Enable authentic MM/MMM wet musette detuning with bit-accurate dry path");
    musetteToggle.onClick = [this] {
      slot.getStrip().musetteEnabled.store(musetteToggle.getToggleState());
    };

    addAndMakeVisible(musetteModeToggle);
    updateMusetteModeButton();
    musetteModeToggle.onClick = [this] {
      int next = (slot.getStrip().musetteMode.load() + 1) % 3;
      slot.getStrip().musetteMode.store(next);
      slot.getStrip().musette3Reed.store(next == 1);
      updateMusetteModeButton();
    };

    addAndMakeVisible(musetteCentsKnob);
    musetteCentsKnob.setRange(3.0, 12.0, 0.5);
    musetteCentsKnob.setValue(slot.getStrip().musetteCents.load(), juce::dontSendNotification);
    musetteCentsKnob.setDoubleClickReturnValue(true, 6.0);
    musetteCentsKnob.setTooltip("Detune offset: Default +6 cents (range +3 to +12 cents)");
    musetteCentsKnob.onValueChange = [this] {
      slot.getStrip().musetteCents.store((float)musetteCentsKnob.getValue());
    };

    addAndMakeVisible(musetteMixKnob);
    musetteMixKnob.setRange(0.0, 1.0, 0.01);
    musetteMixKnob.setValue(slot.getStrip().musetteMix.load(), juce::dontSendNotification);
    musetteMixKnob.setDoubleClickReturnValue(true, 0.25);
    musetteMixKnob.setTooltip("Wet Mix Blend (Baseline: 25% Wet / 75% Dry)");
    musetteMixKnob.onValueChange = [this] {
      slot.getStrip().musetteMix.store((float)musetteMixKnob.getValue());
    };

    // --- DELAY / ECHO (STEREO, PING-PONG, MONO, TAPE WARM) ---
    addAndMakeVisible(delayGroup);
    styleGroup(delayGroup, "DELAY / ECHO");

    addAndMakeVisible(delayToggle);
    delayToggle.setButtonText("ENABLE");
    delayToggle.setClickingTogglesState(true);
    delayToggle.getProperties().set("useToggleSwitch", true);
    delayToggle.getProperties().set("isOrangeToggle", false);
    delayToggle.setToggleState(slot.getStrip().delayEnabled.load(), juce::dontSendNotification);
    delayToggle.setTooltip("Enable per-strip Delay / Echo");
    delayToggle.onClick = [this] {
      slot.getStrip().delayEnabled.store(delayToggle.getToggleState());
    };

    addAndMakeVisible(delayModeBtn);
    updateDelayModeButton();
    delayModeBtn.onClick = [this] {
      int next = (slot.getStrip().delayMode.load() + 1) % 4;
      slot.getStrip().delayMode.store(next);
      updateDelayModeButton();
    };

    addAndMakeVisible(delayTapBtn);
    delayTapBtn.setButtonText("TAP TEMPO");
    Fanfare::setButtonRole(delayTapBtn, Fanfare::ButtonRole::Standard);
    delayTapBtn.setTooltip("Click rhythmically (2-4 taps) to set Delay Time to tempo");
    delayTapBtn.onClick = [this] { handleDelayTapTempo(); };

    addAndMakeVisible(delayTimeLabel);
    styleMiniHeader(delayTimeLabel, "TIME", ThemeManager::get(Theme::Role::engrave));

    addAndMakeVisible(delayTimeKnob);
    delayTimeKnob.setRange(10.0, 2000.0, 1.0);
    delayTimeKnob.setSkewFactorFromMidPoint(350.0);
    delayTimeKnob.setDoubleClickReturnValue(true, 375.0);
    delayTimeKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 84, 18);
    delayTimeKnob.textFromValueFunction = [](double val) {
      if (val >= 1000.0)
        return juce::String(val * 0.001, 2) + " s";
      return juce::String(juce::roundToInt(val)) + " ms";
    };
    delayTimeKnob.valueFromTextFunction = [](const juce::String &txt) {
      auto t = txt.trim();
      double v = t.retainCharacters("0123456789.").getDoubleValue();
      if (t.endsWithIgnoreCase("s") && !t.endsWithIgnoreCase("ms"))
        v *= 1000.0;
      return juce::jlimit(10.0, 2000.0, v);
    };
    delayTimeKnob.setValue(slot.getStrip().delayTimeMs.load(), juce::dontSendNotification);
    delayTimeKnob.setTooltip("Delay Time (10 ms to 2.00 s; double-click for 375 ms)");
    delayTimeKnob.onValueChange = [this] {
      slot.getStrip().delayTimeMs.store((float)delayTimeKnob.getValue());
    };

    addAndMakeVisible(delayFbLabel);
    styleMiniHeader(delayFbLabel, "FEEDBACK", ThemeManager::get(Theme::Role::engrave));

    addAndMakeVisible(delayFbKnob);
    delayFbKnob.setRange(0.0, 0.92, 0.01);
    delayFbKnob.setDoubleClickReturnValue(true, 0.35);
    delayFbKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 76, 18);
    delayFbKnob.textFromValueFunction = [](double val) {
      return juce::String(juce::roundToInt(val * 100.0)) + "%";
    };
    delayFbKnob.valueFromTextFunction = [](const juce::String &txt) {
      return juce::jlimit(0.0, 0.92, txt.retainCharacters("0123456789.").getDoubleValue() * 0.01);
    };
    delayFbKnob.setValue(slot.getStrip().delayFeedback.load(), juce::dontSendNotification);
    delayFbKnob.setTooltip("Delay Feedback / Repeats (0% to 92%; soft-saturated so repeats stay controlled)");
    delayFbKnob.onValueChange = [this] {
      slot.getStrip().delayFeedback.store((float)delayFbKnob.getValue());
    };

    addAndMakeVisible(delayToneLabel);
    styleMiniHeader(delayToneLabel, "TONE (LPF)", ThemeManager::get(Theme::Role::engrave));

    addAndMakeVisible(delayToneKnob);
    delayToneKnob.setRange(600.0, 16000.0, 10.0);
    delayToneKnob.setSkewFactorFromMidPoint(4000.0);
    delayToneKnob.setDoubleClickReturnValue(true, 6500.0);
    delayToneKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 84, 18);
    delayToneKnob.textFromValueFunction = [](double val) {
      if (val >= 1000.0)
        return juce::String(val * 0.001, 1) + " kHz";
      return juce::String(juce::roundToInt(val)) + " Hz";
    };
    delayToneKnob.valueFromTextFunction = [](const juce::String &txt) {
      auto t = txt.trim();
      double v = t.retainCharacters("0123456789.").getDoubleValue();
      if (t.containsIgnoreCase("k"))
        v *= 1000.0;
      return juce::jlimit(600.0, 16000.0, v);
    };
    delayToneKnob.setValue(slot.getStrip().delayTone.load(), juce::dontSendNotification);
    delayToneKnob.setTooltip("Repeat High-Cut Damping Filter (lower = darker warm echoes, higher = crisp repeats)");
    delayToneKnob.onValueChange = [this] {
      slot.getStrip().delayTone.store((float)delayToneKnob.getValue());
    };

    addAndMakeVisible(delayMixLabel);
    styleMiniHeader(delayMixLabel, "MIX", ThemeManager::get(Theme::Role::engrave));

    addAndMakeVisible(delayMixKnob);
    delayMixKnob.setRange(0.0, 1.0, 0.01);
    delayMixKnob.setDoubleClickReturnValue(true, 0.25);
    delayMixKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 76, 18);
    delayMixKnob.textFromValueFunction = [](double val) {
      return juce::String(juce::roundToInt(val * 100.0)) + "%";
    };
    delayMixKnob.valueFromTextFunction = [](const juce::String &txt) {
      return juce::jlimit(0.0, 1.0, txt.retainCharacters("0123456789.").getDoubleValue() * 0.01);
    };
    delayMixKnob.setValue(slot.getStrip().delayMix.load(), juce::dontSendNotification);
    delayMixKnob.setTooltip("Delay Wet Mix (maintains full dry level up to 50%)");
    delayMixKnob.onValueChange = [this] {
      slot.getStrip().delayMix.store((float)delayMixKnob.getValue());
    };

    setSize(980, 520);
  }

  void updateDelayModeButton() {
    int mode = slot.getStrip().delayMode.load();
    switch (mode) {
      case 1:
        delayModeBtn.setButtonText("PING-PONG");
        Fanfare::setButtonRole(delayModeBtn, Fanfare::ButtonRole::Primary);
        delayModeBtn.setTooltip("Delay Mode: Stereo Ping-Pong (bounces Left -> Right -> Left -> Right)");
        break;
      case 2:
        delayModeBtn.setButtonText("MONO");
        Fanfare::setButtonRole(delayModeBtn, Fanfare::ButtonRole::Standard);
        delayModeBtn.setTooltip("Delay Mode: Centered Mono Delay");
        break;
      case 3:
        delayModeBtn.setButtonText("TAPE WARM");
        Fanfare::setButtonRole(delayModeBtn, Fanfare::ButtonRole::Foh);
        delayModeBtn.setTooltip("Delay Mode: Modulated Warm Tape Echo with gentle wow/flutter & soft saturation");
        break;
      default:
        delayModeBtn.setButtonText("STEREO");
        Fanfare::setButtonRole(delayModeBtn, Fanfare::ButtonRole::Primary);
        delayModeBtn.setTooltip("Delay Mode: True Stereo Delay (click to cycle Stereo -> Ping-Pong -> Mono -> Tape Warm)");
        break;
    }
    delayModeBtn.repaint();
  }

  void handleDelayTapTempo() {
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (lastDelayTapMs > 0.0) {
      const double dt = nowMs - lastDelayTapMs;
      if (dt >= 80.0 && dt <= 2000.0) {
        tapIntervals[tapIntervalWriteIdx % 4] = dt;
        ++tapIntervalWriteIdx;
        if (tapIntervalCount < 4)
          ++tapIntervalCount;
        double sum = 0.0;
        for (int i = 0; i < tapIntervalCount; ++i)
          sum += tapIntervals[i];
        const double avgMs = juce::jlimit(10.0, 2000.0, sum / (double)tapIntervalCount);
        delayTimeKnob.setValue(avgMs, juce::sendNotificationSync);
      } else if (dt > 2000.0) {
        tapIntervalCount = 0;
        tapIntervalWriteIdx = 0;
      }
    }
    lastDelayTapMs = nowMs;
  }

  void updateMusetteModeButton() {
    int mode = slot.getStrip().musetteMode.load();
    switch (mode) {
      case 1:
        musetteModeToggle.setButtonText("3-REED MMM");
        Fanfare::setButtonRole(musetteModeToggle, Fanfare::ButtonRole::Primary);
        musetteModeToggle.setTooltip("Musette Mode: 3-Reed MMM (+cents sharp & -4ct flat terzetti)");
        break;
      case 2:
        musetteModeToggle.setButtonText("TEMU MUSETTE");
        Fanfare::setButtonRole(musetteModeToggle, Fanfare::ButtonRole::Help);
        musetteModeToggle.setTooltip("Musette Mode: TEMU Musette (wild bargain-bin extreme detuning for comedic wobble)");
        break;
      default:
        musetteModeToggle.setButtonText("2-REED MM");
        Fanfare::setButtonRole(musetteModeToggle, Fanfare::ButtonRole::Standard);
        musetteModeToggle.setTooltip("Musette Mode: 2-Reed MM (+cents sharp only)");
        break;
    }
    musetteModeToggle.repaint();
  }

  void updateStereoMonoButton() {
    int mode = slot.getStrip().channelMode.load();
    switch (mode) {
      case 1:
        stereoMonoBtn.setButtonText("MONO (L+R)");
        Fanfare::setButtonRole(stereoMonoBtn, Fanfare::ButtonRole::Primary);
        break;
      case 2:
        stereoMonoBtn.setButtonText("MONO (LEFT)");
        Fanfare::setButtonRole(stereoMonoBtn, Fanfare::ButtonRole::Primary);
        break;
      case 3:
        stereoMonoBtn.setButtonText("MONO (RIGHT)");
        Fanfare::setButtonRole(stereoMonoBtn, Fanfare::ButtonRole::Primary);
        break;
      default:
        stereoMonoBtn.setButtonText("STEREO");
        Fanfare::setButtonRole(stereoMonoBtn, Fanfare::ButtonRole::Standard);
        break;
    }
    stereoMonoBtn.repaint();
  }

  void updateEqCurveModeButton() {
    const bool curve = slot.getStrip().eqCurveMode.load();
    if (curve) {
      eqCurveModeBtn.setButtonText("CURVE LINK");
      Fanfare::setButtonRole(eqCurveModeBtn, Fanfare::ButtonRole::Primary);
      eqCurveModeBtn.setTooltip(
          "EQ Slider Mode: Proportional Curve (moving a band smoothly pulls nearby bands in a bell curve; hold Alt for single-band tweak). Click for Independent mode.");
    } else {
      eqCurveModeBtn.setButtonText("INDEPENDENT");
      Fanfare::setButtonRole(eqCurveModeBtn, Fanfare::ButtonRole::Standard);
      eqCurveModeBtn.setTooltip(
          "EQ Slider Mode: Independent (each band moves on its own). Click to enable Proportional Curve mode.");
    }
    eqCurveModeBtn.repaint();
  }

  void showColorPicker() {
    auto *colourSelector = new juce::ColourSelector(juce::ColourSelector::showColourspace);
    colourSelector->setCurrentColour(slot.getChannelColor().withAlpha((juce::uint8)255));
    colourSelector->setSize(300, 400);

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Select Strip Color";
    options.dialogBackgroundColour = ThemeManager::get(Theme::Role::panel);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.content.setOwned(colourSelector);

    juce::Component::SafePointer<ChannelStripComponent> safe(this);
    auto *listener = new ColourChangeListener(safe);
    colourSelector->addChildComponent(listener);
    colourSelector->addChangeListener(listener);

    options.launchAsync();
  }

  void changeListenerCallback(juce::ChangeBroadcaster *source) override {
    if (auto *cs = dynamic_cast<juce::ColourSelector *>(source)) {
      slot.setChannelColor(cs->getCurrentColour());
      colorPreview.setColour(juce::Label::backgroundColourId, slot.getChannelColor());
    }
  }

  void paint(juce::Graphics &g) override {
    Fanfare::ModalFrame::paintOverlayScrimAndPanel(
        g, getLocalBounds(), getLocalBounds(),
        "STRIP DSP // " + slot.getName().toUpperCase(), 48);
  }

  void resized() override {
    closeBtn.setBounds(getWidth() - 68, 11, 56, 26);
    helpBtn.setBounds(getWidth() - 150, 11, 74, 26);
    stereoMonoBtn.setBounds(getWidth() - 298, 11, 140, 26);
    int sectionY = 60;
    int sectionH = 200;

    int gateW = 140;
    int eqW = 440;
    int compW = 170;
    int revW = 170;

    int gateX = 10;
    int eqX = gateX + gateW + 10;
    int compX = eqX + eqW + 10;
    int revX = compX + compW + 10;

    // Noise Gate
    gateGroup.setBounds(gateX, sectionY, gateW, sectionH);
    gateToggle.setBounds(gateX + 10, sectionY + 20, gateW - 20, 26);
    gateThreshKnob.setBounds(gateX + 10, sectionY + 60, gateW - 20, 110);

    // 10-Band Graphic EQ
    eqGroup.setBounds(eqX, sectionY, eqW, sectionH);
    eqToggle.setBounds(eqX + 10, sectionY + 20, 75, 24);
    flatBtn.setBounds(eqX + 90, sectionY + 20, 55, 24);
    eqCurveModeBtn.setBounds(eqX + 150, sectionY + 20, 112, 24);

    int numSliders = 12;
    int sliderW = (eqW - 20) / numSliders;
    int sliderY = sectionY + 70;
    int sliderH = 115;
    int labelY = sectionY + 48;

    eqVolLabel.setBounds(eqX + 10, labelY, sliderW, 20);
    eqVolSlider.setBounds(eqX + 10, sliderY, sliderW, sliderH);

    for (int i = 0; i < 10; ++i) {
      int sx = eqX + 10 + (i + 1) * sliderW;
      if (eqLabels[i]) eqLabels[i]->setBounds(sx, labelY, sliderW, 20);
      if (eqSliders[i]) eqSliders[i]->setBounds(sx, sliderY, sliderW, sliderH);
    }

    int masterX = eqX + 10 + 11 * sliderW;
    eqGainLabel.setBounds(masterX, labelY, sliderW, 20);
    eqGainSlider.setBounds(masterX, sliderY, sliderW, sliderH);

    // Compressor
    compGroup.setBounds(compX, sectionY, compW, sectionH);
    compToggle.setBounds(compX + 10, sectionY + 20, compW - 20, 24);
    compPresetCombo.setBounds(compX + 10, sectionY + 48, compW - 20, 22);

    int singleKnobW = (compW - 55) / 2;
    compAmtKnob.setBounds(compX + 10, sectionY + 75, singleKnobW, 110);
    compMakeupKnob.setBounds(compX + 10 + singleKnobW, sectionY + 75, singleKnobW, 110);
    grMeter.setBounds(compX + compW - 30, sectionY + 48, 20, 137);

    // Reverb / Chorus
    revChoGroup.setBounds(revX, sectionY, revW, sectionH);
    reverbToggle.setBounds(revX + 10, sectionY + 20, revW - 20, 24);
    reverbSizeKnob.setBounds(revX + 10, sectionY + 46, (revW - 30) / 2, 55);
    reverbMixKnob.setBounds(revX + 10 + (revW - 30) / 2, sectionY + 46, (revW - 30) / 2, 55);

    chorusToggle.setBounds(revX + 10, sectionY + 106, revW - 20, 24);
    chorusRateKnob.setBounds(revX + 10, sectionY + 132, (revW - 30) / 2, 55);
    chorusMixKnob.setBounds(revX + 10 + (revW - 30) / 2, sectionY + 132, (revW - 30) / 2, 55);

    int custY = sectionY + sectionH + 18;
    nameLabel.setBounds(20, custY, 95, 24);
    nameEditor.setBounds(120, custY, 155, 24);
    colorLabel.setBounds(20, custY + 36, 95, 24);
    colorPreview.setBounds(120, custY + 36, 40, 24);
    colorButton.setBounds(165, custY + 36, 110, 24);

    int musetteX = 290;
    int musetteW = 330;
    musetteGroup.setBounds(musetteX, custY - 10, musetteW, 96);
    musetteToggle.setBounds(musetteX + 10, custY + 12, 100, 24);
    musetteModeToggle.setBounds(musetteX + 10, custY + 44, 100, 24);
    musetteCentsKnob.setBounds(musetteX + 115, custY + 8, 95, 72);
    musetteMixKnob.setBounds(musetteX + 220, custY + 8, 95, 72);

    const int auxX = 630;
    const int auxW = getWidth() - auxX - 10;
    auxGroup.setBounds(auxX, custY - 10, auxW, 96);
    const int colW = (auxW - 24) / 3;
    const int c0 = auxX + 8;
    const int c1 = c0 + colW + 4;
    const int c2 = c1 + colW + 4;
    iemLevelLabel.setBounds(c0, custY + 4, colW, 14);
    iemLevelKnob.setBounds(c0, custY + 19, colW, 64);
    aux1Label.setBounds(c1, custY + 4, colW, 14);
    aux1Knob.setBounds(c1, custY + 19, colW, 64);
    aux2Label.setBounds(c2, custY + 4, colW, 14);
    aux2Knob.setBounds(c2, custY + 19, colW, 64);

    // Row 3: Stereo / Ping-Pong / Mono / Tape Delay (left) + Convolution IR (right)
    const int row3Y = custY + 94;
    const int row3H = 128;
    const int delayX = 10;
    const int delayW = 610;
    delayGroup.setBounds(delayX, row3Y, delayW, row3H);
    delayToggle.setBounds(delayX + 12, row3Y + 22, 114, 24);
    delayModeBtn.setBounds(delayX + 12, row3Y + 54, 114, 25);
    delayTapBtn.setBounds(delayX + 12, row3Y + 86, 114, 25);

    const int dKnobsX = delayX + 138;
    const int dKnobW = (delayW - 148 - 12) / 4;
    for (int k = 0; k < 4; ++k) {
      const int kx = dKnobsX + k * (dKnobW + 4);
      juce::Label *lbls[4] = {&delayTimeLabel, &delayFbLabel, &delayToneLabel, &delayMixLabel};
      BigKnob *knobs[4] = {&delayTimeKnob, &delayFbKnob, &delayToneKnob, &delayMixKnob};
      lbls[k]->setBounds(kx, row3Y + 18, dKnobW, 14);
      knobs[k]->setBounds(kx, row3Y + 34, dKnobW, 82);
    }

    irGroup.setBounds(auxX, row3Y, auxW, row3H);
    irToggle.setBounds(auxX + 12, row3Y + 22, 100, 24);
    irLoadBtn.setBounds(auxX + 118, row3Y + 22, 102, 24);
    irPresetCombo.setBounds(auxX + 12, row3Y + 54, 208, 25);
    irNameLabel.setBounds(auxX + 12, row3Y + 86, 208, 24);
    irMixLabel.setBounds(auxX + auxW - 108, row3Y + 18, 96, 14);
    irMixKnob.setBounds(auxX + auxW - 108, row3Y + 34, 96, 82);
  }

  void lookAndFeelChanged() override {
    auto refreshGrp = [](juce::GroupComponent &grp) {
      grp.setColour(juce::GroupComponent::outlineColourId,
                    ThemeManager::get(Theme::Role::hairlineStrong));
      grp.setColour(juce::GroupComponent::textColourId,
                    ThemeManager::get(Theme::Role::engrave));
    };
    refreshGrp(gateGroup);
    refreshGrp(eqGroup);
    refreshGrp(compGroup);
    refreshGrp(revChoGroup);
    refreshGrp(musetteGroup);
    refreshGrp(auxGroup);
    refreshGrp(delayGroup);
    refreshGrp(irGroup);
    iemLevelLabel.setColour(juce::Label::textColourId,
                            ThemeManager::get(Theme::Role::iem));
    aux1Label.setColour(juce::Label::textColourId,
                        ThemeManager::get(Theme::Role::engrave));
    aux2Label.setColour(juce::Label::textColourId,
                        ThemeManager::get(Theme::Role::engrave));
    delayTimeLabel.setColour(juce::Label::textColourId,
                             ThemeManager::get(Theme::Role::engrave));
    delayFbLabel.setColour(juce::Label::textColourId,
                           ThemeManager::get(Theme::Role::engrave));
    delayToneLabel.setColour(juce::Label::textColourId,
                             ThemeManager::get(Theme::Role::engrave));
    delayMixLabel.setColour(juce::Label::textColourId,
                            ThemeManager::get(Theme::Role::engrave));
    irMixLabel.setColour(juce::Label::textColourId,
                         ThemeManager::get(Theme::Role::engrave));
    nameLabel.setColour(juce::Label::textColourId,
                        ThemeManager::get(Theme::Role::engrave));
    colorLabel.setColour(juce::Label::textColourId,
                         ThemeManager::get(Theme::Role::engrave));
    irNameLabel.setColour(juce::Label::textColourId,
                          ThemeManager::get(Theme::Role::textDim));
  }

private:
  struct ColourChangeListener : public juce::Component,
                                public juce::ChangeListener {
    juce::Component::SafePointer<ChannelStripComponent> owner;
    explicit ColourChangeListener(juce::Component::SafePointer<ChannelStripComponent> o)
        : owner(o) {}
    void changeListenerCallback(juce::ChangeBroadcaster *source) override {
      if (owner != nullptr)
        owner->changeListenerCallback(source);
    }
  };

  RackSlot &slot;
  std::function<void()> onCloseRequest;
  juce::TextButton closeBtn;
  juce::TextButton helpBtn{"HAAALP!"};
  juce::GroupComponent gateGroup;
  juce::TextButton gateToggle;
  BigKnob gateThreshKnob{" dB"};
  juce::GroupComponent eqGroup;
  juce::TextButton eqToggle;
  juce::TextButton flatBtn;
  juce::TextButton eqCurveModeBtn;
  bool isUpdatingEqCurve = false;
  int activeDragBand = -1;
  double dragAnchorBands[10] = {};
  double lastEqBandValues[10] = {};
  juce::Slider eqVolSlider;
  juce::Label eqVolLabel;
  std::unique_ptr<juce::Slider> eqSliders[10];
  std::unique_ptr<juce::Label> eqLabels[10];
  juce::Slider eqGainSlider;
  juce::Label eqGainLabel;

  juce::GroupComponent compGroup;
  juce::TextButton compToggle;
  juce::ComboBox compPresetCombo;
  BigKnob compAmtKnob{" %"};
  BigKnob compMakeupKnob{" dB"};
  GainReductionMeter grMeter;

  // Space / Mod
  juce::GroupComponent revChoGroup;
  juce::TextButton reverbToggle;
  BigKnob reverbSizeKnob{" Size"};
  BigKnob reverbMixKnob{" Mix"};
  juce::TextButton chorusToggle;
  BigKnob chorusRateKnob{" Hz"};
  BigKnob chorusMixKnob{" Mix"};

  // IR / convolution reverb
  juce::GroupComponent irGroup;
  juce::TextButton irToggle;
  juce::TextButton irLoadBtn{"LOAD WAV"};
  juce::ComboBox irPresetCombo;
  juce::Label irMixLabel;
  BigKnob irMixKnob{""};
  juce::Label irNameLabel{"irNameLabel", "(no IR loaded)"};
  std::unique_ptr<juce::FileChooser> irChooser;

  // Delay / Echo UI
  juce::GroupComponent delayGroup;
  juce::TextButton delayToggle;
  juce::TextButton delayModeBtn;
  juce::TextButton delayTapBtn;
  juce::Label delayTimeLabel;
  BigKnob delayTimeKnob{""};
  juce::Label delayFbLabel;
  BigKnob delayFbKnob{""};
  juce::Label delayToneLabel;
  BigKnob delayToneKnob{""};
  juce::Label delayMixLabel;
  BigKnob delayMixKnob{""};
  double lastDelayTapMs = 0.0;
  double tapIntervals[4]{375.0, 375.0, 375.0, 375.0};
  int tapIntervalCount = 0;
  int tapIntervalWriteIdx = 0;

  juce::Label nameLabel;
  juce::TextEditor nameEditor;
  juce::Label colorLabel;
  juce::Label colorPreview;
  juce::TextButton colorButton;

  juce::GroupComponent auxGroup;
  juce::Label iemLevelLabel;
  BigKnob iemLevelKnob{""};
  juce::Label aux1Label;
  BigKnob aux1Knob{""};
  juce::Label aux2Label;
  BigKnob aux2Knob{""};

  // Musette Detuner UI
  juce::GroupComponent musetteGroup;
  juce::TextButton musetteToggle;
  juce::TextButton musetteModeToggle;
  BigKnob musetteCentsKnob{" ct"};
  BigKnob musetteMixKnob{" Mix"};

  // Input Channel Mode (Stereo / Mono)
  juce::TextButton stereoMonoBtn;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChannelStripComponent)
};
