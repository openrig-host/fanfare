#include "ConfigModalOverlay.h"
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "HelpLauncher.h"
#include "ThemeManager.h"
#include "Version.h"

ConfigModalOverlay::ConfigModalOverlay(Actions a) : actions(std::move(a)) {
  // Title is drawn as an engraved header in ModalFrame::paintOverlayScrimAndPanel
  titleLabel.setVisible(false);

  infoLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
  infoLabel.setFont(BoutiqueLookAndFeel::monoFont(11.5f, true));
  infoLabel.setJustificationType(juce::Justification::centredLeft);
  addAndMakeVisible(infoLabel);

  themeLabel.setFont(BoutiqueLookAndFeel::condensedFont(12.0f, 0.08f));
  themeLabel.setJustificationType(juce::Justification::centredLeft);
  addAndMakeVisible(themeLabel);

  themeCombo.addItemList(ThemeManager::getInstance().getThemeNames(), 1);
  themeCombo.setSelectedItemIndex(ThemeManager::getInstance().getCurrentIndex(),
                                  juce::dontSendNotification);
  themeCombo.onChange = [this] {
    ThemeManager::getInstance().setCurrentByIndex(themeCombo.getSelectedItemIndex());
    lookAndFeelChanged();
    themeCombo.sendLookAndFeelChange();
    repaint();
  };
  addAndMakeVisible(themeCombo);
  lookAndFeelChanged();

  addSettingButton(audioSettingsBtn, "AUDIO & MIDI SETTINGS");
  Fanfare::setButtonRole(audioSettingsBtn, Fanfare::ButtonRole::Primary);
  audioSettingsBtn.setTooltip("Low-latency audio driver configuration, buffer size & sample rates.");
  audioSettingsBtn.onClick = [this] { clicked(actions.onAudioSettings); };

  addSettingButton(engineTuningBtn, "ENGINE & CPU TUNING");
  engineTuningBtn.setTooltip("CPU core affinity, CPU 0 sheltering, Windows power throttling & parallel DSP processing.");
  engineTuningBtn.onClick = [this] { clicked(actions.onEngineTuning); };

  addSettingButton(scanPluginsBtn, "SCAN FOR PLUGINS");
  Fanfare::setButtonRole(scanPluginsBtn, Fanfare::ButtonRole::Primary);
  scanPluginsBtn.setTooltip("Scans system VST3 directories to register installed instruments & effects.");
  scanPluginsBtn.onClick = [this] { clicked(actions.onScanPlugins); };

  addSettingButton(busRoutingBtn, "BUS ROUTING");
  busRoutingBtn.setTooltip("Configure FOH (Front of House) and IEM (In-Ear Monitor) output channel offsets.");
  busRoutingBtn.onClick = [this] { clicked(actions.onBusRouting); };

  addSettingButton(resetAudioBtn, "RESET AUDIO DRIVER");
  Fanfare::setButtonRole(resetAudioBtn, Fanfare::ButtonRole::Danger);
  resetAudioBtn.setTooltip("Re-initializes audio driver threads and clears sample-rate locks mid-gig.");
  resetAudioBtn.onClick = [this] { clicked(actions.onResetAudio); };

  addSettingButton(stressTestBtn, "RUN STRESS TEST");
  stressTestBtn.setTooltip("Simulates rapid scene switching and max CPU load to guarantee your rig won't crash mid-solo.");
  stressTestBtn.onClick = [this] {
    clicked(actions.onStressTestToggle);
    updateStressTestButtonState();
  };

  addSettingButton(aboutBtn, "ABOUT FANFARE (v" OPENRIG_VERSION_STRING ")");
  aboutBtn.setTooltip("Fanfare Stage Engine philosophy, version & stage credits.");
  aboutBtn.onClick = [this] { clicked(actions.onAbout); };

  closeBtn.setButtonText("CLOSE");
  Fanfare::setButtonRole(closeBtn, Fanfare::ButtonRole::Subtle);
  closeBtn.onClick = [this] {
    if (actions.onClose)
      actions.onClose();
  };
  addAndMakeVisible(closeBtn);

  Fanfare::styleHelpButton(helpBtn, "#settings", "HAAALP! Open the Configuration & Settings section of the User's Manual");
  addAndMakeVisible(helpBtn);

  refreshInfo();
  updateStressTestButtonState();
  setSize(376, 488);
}

void ConfigModalOverlay::updateStressTestButtonState() {
  const bool running = (actions.isStressTestActive && actions.isStressTestActive());
  stressTestBtn.setToggleState(running, juce::dontSendNotification);
  if (running) {
    stressTestBtn.setButtonText("STOP STRESS TEST");
    Fanfare::setButtonRole(stressTestBtn, Fanfare::ButtonRole::Mute);
  } else {
    stressTestBtn.setButtonText("RUN STRESS TEST");
    Fanfare::setButtonRole(stressTestBtn, Fanfare::ButtonRole::Standard);
  }
}

void ConfigModalOverlay::refreshInfo() {
  if (actions.getInfo)
    infoLabel.setText(actions.getInfo(), juce::dontSendNotification);
}

void ConfigModalOverlay::paint(juce::Graphics &g) {
  Fanfare::ModalFrame::paintOverlayScrimAndPanel(
      g, getLocalBounds(), getLocalBounds(), "CONFIGURATION", 38);

  if (!infoLabel.getBounds().isEmpty()) {
    Fanfare::ModalFrame::paintRecessedWell(g, infoLabel.getBounds().toFloat().expanded(2.0f, 1.0f), 2.5f);
  }
}

void ConfigModalOverlay::resized() {
  auto full = getLocalBounds();
  auto top = full.removeFromTop(38).reduced(10, 6);
  closeBtn.setBounds(top.removeFromRight(52));
  top.removeFromRight(4);
  helpBtn.setBounds(top.removeFromRight(68));

  auto area = full.reduced(14, 10);
  infoLabel.setBounds(area.removeFromTop(62).reduced(6, 2));
  area.removeFromTop(8);

  auto themeRow = area.removeFromTop(28);
  themeLabel.setBounds(themeRow.removeFromLeft(66));
  themeCombo.setBounds(themeRow);
  area.removeFromTop(8);

  const int rowH = 34;
  audioSettingsBtn.setBounds(area.removeFromTop(rowH).reduced(0, 2));
  engineTuningBtn.setBounds(area.removeFromTop(rowH).reduced(0, 2));
  scanPluginsBtn.setBounds(area.removeFromTop(rowH).reduced(0, 2));
  busRoutingBtn.setBounds(area.removeFromTop(rowH).reduced(0, 2));
  resetAudioBtn.setBounds(area.removeFromTop(rowH).reduced(0, 2));
  stressTestBtn.setBounds(area.removeFromTop(rowH).reduced(0, 2));
  aboutBtn.setBounds(area.removeFromTop(rowH).reduced(0, 2));
}

void ConfigModalOverlay::addSettingButton(juce::TextButton &b, const juce::String &text) {
  b.setButtonText(text);
  Fanfare::setButtonRole(b, Fanfare::ButtonRole::Standard);
  addAndMakeVisible(b);
}

void ConfigModalOverlay::lookAndFeelChanged() {
  const auto& t = ThemeManager::getInstance().active();
  infoLabel.setColour(juce::Label::textColourId, t.dark ? t.textDim : t.text);
  themeLabel.setColour(juce::Label::textColourId, t.engrave);
}

void ConfigModalOverlay::clicked(std::function<void()> &cb) {
  if (cb)
    cb();
  refreshInfo();
}
