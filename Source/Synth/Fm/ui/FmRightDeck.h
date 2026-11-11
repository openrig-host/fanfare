#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include "../FmSynthProcessor.h"
#include "FmAlgorithmView.h"
#include "FmControls.h"
#include "FmUiMetrics.h"

namespace FanfareFm::Ui {

class FmRightDeck : public juce::Component {
public:
  FmRightDeck(FmSynthProcessor &processor, FmTelemetrySink &telemetry,
              std::function<void()> onPatchCommit)
      : proc(processor), sink(telemetry), commitCallback(std::move(onPatchCommit)),
        fbFader("FB", "Feedback Level", 7, 0),
        volFader("VOL", "Synth Output Volume", 100, 80),
        trspKnob("TRSP", "Transpose", 0.5f, true, 48),
        pageTabs(juce::StringArray{"MOD / PITCH EG", "CHORUS / DLY / REV"},
                 "Switch between LFO / Pitch Envelope and Stereo FX Rack"),
        lfoWaveSwitch(juce::StringArray{"TRI", "SW-", "SW+", "SQR", "SIN", "S&H"},
                      "DX7 LFO Waveform"),
        oscSyncToggle("OSC SYNC", "OSC FREE", Theme::Role::accent,
                      "Reset operator phases on every key-down"),
        lfoSyncToggle("LFO SYNC", "LFO FREE", Theme::Role::accent,
                      "Reset LFO phase on key-down"),
        choModeSwitch(juce::StringArray{"OFF", "I", "II", "I+II", "CUST"},
                      "Juno BBD Chorus Mode"),
        revTypeSwitch(juce::StringArray{"ROOM", "HALL", "PLATE"},
                      "Lush Reverb Algorithm") {
    addAndMakeVisible(algView);
    algView.onAlgorithmSelected = [this](int alg) {
      proc.engine().patch().d[134] = static_cast<uint8_t>(alg);
      sink.handleTelemetryEdited("Algorithm: " + juce::String(alg + 1));
      commitCallback();
    };
    algView.onStepAlgorithm = [this](int delta) { stepAlgorithm(delta); };

    algDownBtn.setButtonText("<");
    algDownBtn.setTooltip("Previous FM Algorithm");
    algDownBtn.onClick = [this] { stepAlgorithm(-1); };
    addAndMakeVisible(algDownBtn);

    algUpBtn.setButtonText(">");
    algUpBtn.setTooltip("Next FM Algorithm");
    algUpBtn.onClick = [this] { stepAlgorithm(1); };
    addAndMakeVisible(algUpBtn);

    fbFader.setTelemetrySink(&sink);
    fbFader.setAccentRole(Theme::Role::warn, Theme::Role::warn);
    fbFader.onValueChanged = [this](int v) {
      proc.engine().patch().d[135] = static_cast<uint8_t>(v);
      commitCallback();
    };
    addAndMakeVisible(fbFader);

    volFader.setTelemetrySink(&sink);
    volFader.formatReadout = [](int v) { return juce::String(v) + "%"; };
    volFader.onValueChanged = [this](int v) {
      proc.setVolume(static_cast<float>(v) / 100.0f);
      proc.markDirty();
    };
    addAndMakeVisible(volFader);

    trspKnob.setTelemetrySink(&sink);
    trspKnob.formatReadout = [](float n) {
      const int st = static_cast<int>(std::lround(n * 48.0f)) - 24;
      return (st > 0 ? "+" : "") + juce::String(st);
    };
    trspKnob.formatTelemetry = [](float n) {
      return "Transpose: " +
             formatTranspose(static_cast<int>(std::lround(n * 48.0f)));
    };
    trspKnob.onNormChanged = [this](float n) {
      proc.engine().patch().d[144] =
          static_cast<uint8_t>(juce::jlimit(0, 48, static_cast<int>(std::lround(n * 48.0f))));
      commitCallback();
    };
    addAndMakeVisible(trspKnob);

    addAndMakeVisible(pageTabs);
    pageTabs.onSelectionChanged = [this](int idx) {
      showingFx = (idx == 1);
      modPage.setVisible(!showingFx);
      fxPage.setVisible(showingFx);
      resized();
      repaint();
    };

    addAndMakeVisible(modPage);
    addAndMakeVisible(fxPage);
    fxPage.setVisible(false);

    buildModPage();
    buildFxPage();
  }

  bool anyDragging() const noexcept {
    if (fbFader.isDraggingNow() || volFader.isDraggingNow() ||
        trspKnob.isDraggingNow()) {
      return true;
    }
    for (const auto &f : lfoFaders)
      if (f && f->isDraggingNow())
        return true;
    for (const auto &f : pegFaders)
      if (f && f->isDraggingNow())
        return true;
    for (const auto &k : fxKnobs)
      if (k && k->isDraggingNow())
        return true;
    return false;
  }

  void syncFromProcessor() {
    const auto &e = proc.engine();
    const auto &p = e.patch();
    const int maxAlgs = fmModeMaxAlg(e.mode());

    algView.setState(e.effectiveAlg(), maxAlgs, e.numOps(), p.d[155]);
    fbFader.setValueSilent(p.d[135]);
    volFader.setValueSilent(static_cast<int>(std::lround(proc.getVolume() * 100.0f)));
    trspKnob.setNormSilent(static_cast<float>(p.d[144]) / 48.0f);

    lfoWaveSwitch.setSelectedIndexSilent(juce::jlimit(0, 5, static_cast<int>(p.d[142])));
    oscSyncToggle.setStateSilent((p.d[136] & 1u) != 0);
    lfoSyncToggle.setStateSilent((p.d[141] & 1u) != 0);

    for (int i = 0; i < 4; ++i)
      lfoFaders[(size_t)i]->setValueSilent(p.d[137 + i]);
    lfoFaders[4]->setValueSilent(p.d[143] & 7);

    for (int i = 0; i < 8; ++i)
      pegFaders[(size_t)i]->setValueSilent(p.d[126 + i]);

    using Fx = FmSynthProcessor::FmFx;
    const int choMode =
        juce::jlimit(0, 4, static_cast<int>(std::lround(proc.getFx(Fx::fxChoMode))));
    choModeSwitch.setSelectedIndexSilent(choMode);
    fxKnobs[0]->setNormSilent(fxRateToNorm(proc.getFx(Fx::fxChoRate)));
    fxKnobs[1]->setNormSilent(proc.getFx(Fx::fxChoDepth));
    fxKnobs[2]->setNormSilent(proc.getFx(Fx::fxChoMix));

    const bool choCustom = (choMode == 4);
    const bool choActive = (choMode != 0);
    fxKnobs[0]->setAlpha(choCustom ? 1.0f : Metrics::kDisabledAlpha);
    fxKnobs[0]->setInterceptsMouseClicks(choCustom, choCustom);
    fxKnobs[1]->setAlpha(choCustom ? 1.0f : Metrics::kDisabledAlpha);
    fxKnobs[1]->setInterceptsMouseClicks(choCustom, choCustom);
    fxKnobs[2]->setAlpha(choActive ? 1.0f : Metrics::kDisabledAlpha);
    fxKnobs[2]->setInterceptsMouseClicks(choActive, choActive);

    fxKnobs[3]->setNormSilent(fxTimeToNorm(proc.getFx(Fx::fxDlyTime)));
    fxKnobs[4]->setNormSilent(proc.getFx(Fx::fxDlyFb));
    fxKnobs[5]->setNormSilent(proc.getFx(Fx::fxDlyDamp));
    fxKnobs[6]->setNormSilent(proc.getFx(Fx::fxDlyMix));

    const int revType =
        juce::jlimit(0, 2, static_cast<int>(std::lround(proc.getFx(Fx::fxRevType))));
    revTypeSwitch.setSelectedIndexSilent(revType);
    fxKnobs[7]->setNormSilent(proc.getFx(Fx::fxRevSize));
    fxKnobs[8]->setNormSilent(proc.getFx(Fx::fxRevDamp));
    fxKnobs[9]->setNormSilent(proc.getFx(Fx::fxRevMix));

    repaint();
  }

  void resized() override {
    auto b = getLocalBounds();
    const int topCardH =
        juce::jlimit(148, 214, static_cast<int>(b.getHeight() * 0.37f));
    algCardRect = b.removeFromTop(topCardH);
    b.removeFromTop(Metrics::kSectionGap);
    bottomCardRect = b;

    auto algHdr = algCardRect.withHeight(Metrics::kSectionHeaderHeight).reduced(4, 1);
    algUpBtn.setBounds(algHdr.removeFromRight(20));
    algHdr.removeFromRight(2);
    algNumRect = algHdr.removeFromRight(46);
    algHdr.removeFromRight(2);
    algDownBtn.setBounds(algHdr.removeFromRight(20));

    auto algBody =
        algCardRect.withTrimmedTop(Metrics::kSectionHeaderHeight).reduced(5, 4);
    const int masterColW = juce::jlimit(84, 98, static_cast<int>(algBody.getWidth() * 0.36f));
    auto masterCol = algBody.removeFromRight(masterColW);
    algBody.removeFromRight(5);
    algView.setBounds(algBody);

    const int knobH = juce::jlimit(48, 62, static_cast<int>(masterCol.getHeight() * 0.43f));
    auto trspArea = masterCol.removeFromBottom(knobH);
    masterCol.removeFromBottom(3);
    const int halfW = (masterCol.getWidth() - 3) / 2;
    fbFader.setBounds(masterCol.removeFromLeft(halfW));
    masterCol.removeFromLeft(3);
    volFader.setBounds(masterCol);
    trspKnob.setBounds(trspArea);

    auto botInner = bottomCardRect;
    pageTabs.setBounds(botInner.removeFromTop(21).reduced(3, 2));
    modPage.setBounds(botInner);
    fxPage.setBounds(botInner);

    layoutModPage();
    layoutFxPage();
  }

  void paint(juce::Graphics &g) override {
    paintHardwareCardFrame(g, algCardRect, "ALGORITHM");

    const auto &e = proc.engine();
    g.setColour(ThemeManager::get(Theme::Role::accent));
    g.setFont(BoutiqueLookAndFeel::monoFont(10.5f, true));
    g.drawFittedText(juce::String(e.effectiveAlg() + 1).paddedLeft('0', 2) + "/" +
                         juce::String(fmModeMaxAlg(e.mode())),
                     algNumRect, juce::Justification::centred, 1);

    const auto bf = bottomCardRect.toFloat();
    g.setColour(ThemeManager::get(Theme::Role::panel));
    g.fillRoundedRectangle(bf, 3.0f);
    g.setColour(ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(bf.reduced(0.5f), 3.0f, 1.0f);
    g.drawHorizontalLine(bottomCardRect.getY() + 22, bf.getX() + 1.0f,
                         bf.getRight() - 1.0f);
  }

private:
  class CustomPaintContainer : public juce::Component {
  public:
    std::function<void(juce::Graphics &)> onPaint;
    void paint(juce::Graphics &g) override {
      if (onPaint)
        onPaint(g);
    }
  };

  static float fxRateToNorm(float v) noexcept {
    return juce::jlimit(0.0f, 1.0f,
                        std::log(juce::jmax(0.1f, v) / 0.1f) / std::log(80.0f));
  }
  static float fxNormToRate(float n) noexcept {
    return 0.1f * std::pow(80.0f, juce::jlimit(0.0f, 1.0f, n));
  }
  static float fxTimeToNorm(float v) noexcept {
    return juce::jlimit(0.0f, 1.0f,
                        std::log(juce::jmax(0.02f, v) / 0.02f) / std::log(75.0f));
  }
  static float fxNormToTime(float n) noexcept {
    return 0.02f * std::pow(75.0f, juce::jlimit(0.0f, 1.0f, n));
  }

  void stepAlgorithm(int delta) {
    const int mx = fmModeMaxAlg(proc.engine().mode());
    int a = proc.engine().effectiveAlg() + delta;
    if (a < 0)
      a = mx - 1;
    if (a >= mx)
      a = 0;
    proc.engine().patch().d[134] = static_cast<uint8_t>(a);
    sink.handleTelemetryEdited("Algorithm: " + juce::String(a + 1) + "/" +
                               juce::String(mx));
    commitCallback();
  }

  void buildModPage() {
    modPage.onPaint = [this](juce::Graphics &g) {
      g.setColour(ThemeManager::get(Theme::Role::border));
      if (modDividerY > 0)
        g.drawHorizontalLine(modDividerY, 5.0f,
                             static_cast<float>(modPage.getWidth()) - 5.0f);
      if (!pegHeaderRect.isEmpty()) {
        g.setColour(ThemeManager::get(Theme::Role::engrave));
        g.setFont(BoutiqueLookAndFeel::condensedFont(10.5f, 0.05f));
        g.drawText("PITCH ENVELOPE (50 = CENTER)", pegHeaderRect,
                   juce::Justification::centredLeft, false);
      }
    };

    modPage.addAndMakeVisible(lfoWaveSwitch);
    lfoWaveSwitch.onSelectionChanged = [this](int idx) {
      proc.engine().patch().d[142] = static_cast<uint8_t>(idx);
      commitCallback();
    };

    modPage.addAndMakeVisible(oscSyncToggle);
    oscSyncToggle.onToggled = [this](bool on) {
      proc.engine().patch().d[136] = on ? 1 : 0;
      commitCallback();
    };

    modPage.addAndMakeVisible(lfoSyncToggle);
    lfoSyncToggle.onToggled = [this](bool on) {
      proc.engine().patch().d[141] = on ? 1 : 0;
      commitCallback();
    };

    static constexpr const char *kLfoShort[5] = {"RATE", "DLY", "PMD", "AMD",
                                                 "PMS"};
    static constexpr const char *kLfoFull[5] = {
        "LFO Speed", "LFO Delay", "LFO Pitch Mod Depth", "LFO Amp Mod Depth",
        "LFO Pitch Mod Sensitivity"};
    static constexpr const int kLfoMax[5] = {99, 99, 99, 99, 7};
    static constexpr const int kLfoDef[5] = {35, 0, 0, 0, 3};

    for (int i = 0; i < 5; ++i) {
      lfoFaders[(size_t)i] = std::make_unique<FmFader>(
          kLfoShort[i], kLfoFull[i], kLfoMax[i], kLfoDef[i]);
      lfoFaders[(size_t)i]->setTelemetrySink(&sink);
      lfoFaders[(size_t)i]->onValueChanged = [this, i](int v) {
        auto &p = proc.engine().patch();
        if (i < 4)
          p.d[137 + i] = static_cast<uint8_t>(v);
        else
          p.d[143] = static_cast<uint8_t>(v);
        commitCallback();
      };
      modPage.addAndMakeVisible(*lfoFaders[(size_t)i]);
    }

    static constexpr const char *kPegShort[8] = {"PR1", "PR2", "PR3", "PR4",
                                                 "PL1", "PL2", "PL3", "PL4"};
    for (int i = 0; i < 8; ++i) {
      const bool isLevel = (i >= 4);
      pegFaders[(size_t)i] = std::make_unique<FmFader>(
          kPegShort[i],
          juce::String(isLevel ? "Pitch EG Level " : "Pitch EG Rate ") +
              juce::String((i % 4) + 1),
          99, isLevel ? 50 : 99, isLevel, 50);
      pegFaders[(size_t)i]->setTelemetrySink(&sink);
      pegFaders[(size_t)i]->onValueChanged = [this, i](int v) {
        proc.engine().patch().d[126 + i] = static_cast<uint8_t>(v);
        commitCallback();
      };
      modPage.addAndMakeVisible(*pegFaders[(size_t)i]);
    }
  }

  void layoutModPage() {
    auto r = modPage.getLocalBounds().reduced(5, 4);
    if (r.isEmpty())
      return;

    lfoWaveSwitch.setBounds(r.removeFromTop(18));
    r.removeFromTop(3);

    auto syncRow = r.removeFromTop(16);
    const int halfW = (syncRow.getWidth() - 4) / 2;
    oscSyncToggle.setBounds(syncRow.removeFromLeft(halfW));
    syncRow.removeFromLeft(4);
    lfoSyncToggle.setBounds(syncRow);
    r.removeFromTop(4);

    const int availH = juce::jmax(80, r.getHeight() - 18);
    const int lfoH = static_cast<int>(std::lround(availH * 0.47f));
    auto lfoRow = r.removeFromTop(lfoH);
    layoutArrayRow(lfoRow, lfoFaders.data(), 5, 3);

    modDividerY = r.getY() + 2;
    r.removeFromTop(5);
    pegHeaderRect = r.removeFromTop(12);
    r.removeFromTop(2);
    layoutArrayRow(r, pegFaders.data(), 8, 2);
  }

  void buildFxPage() {
    using Fx = FmSynthProcessor::FmFx;

    fxPage.onPaint = [this](juce::Graphics &g) {
      g.setColour(ThemeManager::get(Theme::Role::border));
      if (fxDiv1Y > 0)
        g.drawHorizontalLine(fxDiv1Y, 5.0f,
                             static_cast<float>(fxPage.getWidth()) - 5.0f);
      if (fxDiv2Y > 0)
        g.drawHorizontalLine(fxDiv2Y, 5.0f,
                             static_cast<float>(fxPage.getWidth()) - 5.0f);
      g.setColour(ThemeManager::get(Theme::Role::engrave));
      g.setFont(BoutiqueLookAndFeel::condensedFont(10.0f, 0.05f));
      if (!choLabelRect.isEmpty())
        g.drawText("CHORUS", choLabelRect, juce::Justification::centredLeft, false);
      if (!dlyLabelRect.isEmpty())
        g.drawText("PING-PONG DELAY", dlyLabelRect,
                   juce::Justification::centredLeft, false);
      if (!revLabelRect.isEmpty())
        g.drawText("LUSH REVERB", revLabelRect, juce::Justification::centredLeft,
                   false);
    };

    fxPage.addAndMakeVisible(choModeSwitch);
    choModeSwitch.onSelectionChanged = [this](int m) {
      proc.setFx(Fx::fxChoMode, static_cast<float>(m));
      proc.markDirty();
      syncFromProcessor();
    };

    fxPage.addAndMakeVisible(revTypeSwitch);
    revTypeSwitch.onSelectionChanged = [this](int rt) {
      proc.setFx(Fx::fxRevType, static_cast<float>(rt));
      proc.markDirty();
      syncFromProcessor();
    };

    struct FxSpec {
      const char *shortLbl;
      const char *fullLbl;
      int fxParam;
      float defNorm;
    };
    const FxSpec specs[10] = {
        {"RATE", "Chorus Rate", Fx::fxChoRate, fxRateToNorm(0.65f)},
        {"DEPTH", "Chorus Depth", Fx::fxChoDepth, 0.50f},
        {"MIX", "Chorus Mix", Fx::fxChoMix, 0.50f},
        {"TIME", "Delay Time", Fx::fxDlyTime, fxTimeToNorm(0.32f)},
        {"FDBK", "Delay Feedback", Fx::fxDlyFb, 0.35f},
        {"DAMP", "Delay Damping", Fx::fxDlyDamp, 0.35f},
        {"MIX", "Delay Wet Mix", Fx::fxDlyMix, 0.0f},
        {"SIZE", "Reverb Size", Fx::fxRevSize, 0.55f},
        {"DAMP", "Reverb Damping", Fx::fxRevDamp, 0.45f},
        {"MIX", "Reverb Wet Mix", Fx::fxRevMix, 0.0f},
    };

    for (int i = 0; i < 10; ++i) {
      fxKnobs[(size_t)i] = std::make_unique<FmKnob>(
          specs[i].shortLbl, specs[i].fullLbl, specs[i].defNorm);
      fxKnobs[(size_t)i]->setTelemetrySink(&sink);

      if (i == 0) {
        fxKnobs[0]->formatReadout = [](float n) {
          return juce::String(fxNormToRate(n), 2) + "Hz";
        };
      } else if (i == 3) {
        fxKnobs[3]->formatReadout = [](float n) {
          const float sec = fxNormToTime(n);
          return sec >= 1.0f ? (juce::String(sec, 2) + "s")
                             : (juce::String(static_cast<int>(std::lround(sec * 1000.0f))) + "ms");
        };
      }

      const int paramId = specs[i].fxParam;
      fxKnobs[(size_t)i]->onNormChanged = [this, i, paramId](float n) {
        float val = n;
        if (i == 0)
          val = fxNormToRate(n);
        else if (i == 3)
          val = fxNormToTime(n);
        proc.setFx(paramId, val);
        proc.markDirty();
      };
      fxPage.addAndMakeVisible(*fxKnobs[(size_t)i]);
    }
  }

  void layoutFxPage() {
    auto r = fxPage.getLocalBounds().reduced(5, 4);
    if (r.isEmpty())
      return;

    const int bandH = (r.getHeight() - 8) / 3;

    auto choBand = r.removeFromTop(bandH);
    auto choTop = choBand.removeFromTop(17);
    choLabelRect = choTop.removeFromLeft(46);
    choModeSwitch.setBounds(choTop);
    choBand.removeFromTop(2);
    layoutKnobSubrange(choBand, 0, 3);

    fxDiv1Y = r.getY() + 2;
    r.removeFromTop(4);

    auto dlyBand = r.removeFromTop(bandH);
    dlyLabelRect = dlyBand.removeFromTop(13);
    dlyBand.removeFromTop(2);
    layoutKnobSubrange(dlyBand, 3, 4);

    fxDiv2Y = r.getY() + 2;
    r.removeFromTop(4);

    auto revBand = r;
    auto revTop = revBand.removeFromTop(17);
    revLabelRect = revTop.removeFromLeft(74);
    revTypeSwitch.setBounds(revTop);
    revBand.removeFromTop(2);
    layoutKnobSubrange(revBand, 7, 3);
  }

  template <typename PtrType>
  static void layoutArrayRow(juce::Rectangle<int> row, PtrType *arr, int count,
                             int gap) {
    if (count <= 0 || row.isEmpty())
      return;
    const int availW = juce::jmax(count, row.getWidth() - (count - 1) * gap);
    int curX = row.getX();
    for (int i = 0; i < count; ++i) {
      const int w = (i == count - 1) ? (row.getRight() - curX) : (availW / count);
      arr[i]->setBounds(curX, row.getY(), juce::jmax(10, w), row.getHeight());
      curX += w + gap;
    }
  }

  void layoutKnobSubrange(juce::Rectangle<int> row, int startIdx, int count) {
    layoutArrayRow(row, fxKnobs.data() + startIdx, count, 3);
  }

  FmSynthProcessor &proc;
  FmTelemetrySink &sink;
  std::function<void()> commitCallback;

  FmAlgorithmView algView;
  juce::TextButton algDownBtn, algUpBtn;
  FmFader fbFader, volFader;
  FmKnob trspKnob;

  FmSegmentedSwitch pageTabs;
  CustomPaintContainer modPage, fxPage;
  bool showingFx = false;

  FmSegmentedSwitch lfoWaveSwitch;
  FmLedPillToggle oscSyncToggle, lfoSyncToggle;
  std::array<std::unique_ptr<FmFader>, 5> lfoFaders;
  std::array<std::unique_ptr<FmFader>, 8> pegFaders;

  FmSegmentedSwitch choModeSwitch, revTypeSwitch;
  std::array<std::unique_ptr<FmKnob>, 10> fxKnobs;

  juce::Rectangle<int> algCardRect, algNumRect, bottomCardRect, pegHeaderRect;
  juce::Rectangle<int> choLabelRect, dlyLabelRect, revLabelRect;
  int modDividerY = -1;
  int fxDiv1Y = -1;
  int fxDiv2Y = -1;
};

} // namespace FanfareFm::Ui
