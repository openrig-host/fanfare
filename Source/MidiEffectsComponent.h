#pragma once

#include <JuceHeader.h>
#include "SimpleArpeggiator.h"
#include "OctaveHarmonizer.h"
#include "ButtonRoles.h"
#include "HelpLauncher.h"
#include "ThemeManager.h"
#include "RackSlot.h"

class MidiEffectsComponent : public juce::Component,
                              public juce::Label::Listener {
public:
    MidiEffectsComponent(SimpleArpeggiator& arp, OctaveHarmonizer& harm,
                         const juce::StringArray& slotNames, int ownSlotIndex,
                         RackSlot& slot,
                         std::function<void()> onClose)
        : arpeggiator(arp), harmonizer(harm), slotRef(slot),
          closeCallback(std::move(onClose)),
          transposeDownBtn(juce::translate("Transpose Down"), 0.25f, ThemeManager::get(Theme::Role::accent)),
          transposeUpBtn(juce::translate("Transpose Up"), 0.75f, ThemeManager::get(Theme::Role::accent)),
          arpBpmDownBtn(juce::translate("BPM Down"), 0.25f, ThemeManager::get(Theme::Role::foh)),
          arpBpmUpBtn(juce::translate("BPM Up"), 0.75f, ThemeManager::get(Theme::Role::foh))
    {
        auto styleEngraved = [](juce::Label& lbl, const juce::String& text, float pt = 12.5f) {
            lbl.setText(text, juce::dontSendNotification);
            lbl.setFont(juce::FontOptions("Barlow Condensed", pt, juce::Font::bold));
            lbl.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::engrave));
        };

        // TRANSPOSE strip (full width, above ARP/OCTAVER columns)
        styleEngraved(transposeHeader, "TRANSPOSE", 13.5f);
        transposeHeader.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(transposeHeader);

        transposeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        transposeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        transposeSlider.setRange(-48, 48, 1);
        transposeSlider.setValue(slotRef.getTransposeSemis(), juce::dontSendNotification);
        transposeSlider.onValueChange = [this] {
            int v = (int)transposeSlider.getValue();
            slotRef.setTransposeSemis(v);
            transposeValue.setText((v > 0 ? "+" : "") + juce::String(v) + " st",
                                   juce::dontSendNotification);
        };
        transposeValue.setText(
            [&] {
                int v = slotRef.getTransposeSemis();
                return (v > 0 ? "+" : "") + juce::String(v) + " st";
            }(),
            juce::dontSendNotification);
        transposeValue.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::accent));
        transposeValue.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.5f, juce::Font::bold));
        transposeValue.setJustificationType(juce::Justification::centredRight);

        transposeDownBtn.onClick = [this] {
            int v = juce::jlimit(-48, 48, slotRef.getTransposeSemis() - 1);
            slotRef.setTransposeSemis(v);
            transposeSlider.setValue(v, juce::dontSendNotification);
            transposeValue.setText((v > 0 ? "+" : "") + juce::String(v) + " st",
                                   juce::dontSendNotification);
        };
        transposeUpBtn.onClick = [this] {
            int v = juce::jlimit(-48, 48, slotRef.getTransposeSemis() + 1);
            slotRef.setTransposeSemis(v);
            transposeSlider.setValue(v, juce::dontSendNotification);
            transposeValue.setText((v > 0 ? "+" : "") + juce::String(v) + " st",
                                   juce::dontSendNotification);
        };
        addAndMakeVisible(transposeSlider);
        addAndMakeVisible(transposeValue);
        addAndMakeVisible(transposeDownBtn);
        addAndMakeVisible(transposeUpBtn);

        arpHeader.setText("ARPEGGIATOR", juce::dontSendNotification);
        arpHeader.setFont(juce::FontOptions("Barlow Condensed", 14.0f, juce::Font::bold));
        arpHeader.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::foh));
        addAndMakeVisible(arpHeader);

        arpEnableBtn.setClickingTogglesState(true);
        arpEnableBtn.setToggleState(arpeggiator.enabled.load(), juce::dontSendNotification);
        arpEnableBtn.setButtonText(arpEnableBtn.getToggleState() ? "ON" : "OFF");
        Fanfare::setButtonRole(arpEnableBtn, arpEnableBtn.getToggleState() ? Fanfare::ButtonRole::Primary : Fanfare::ButtonRole::Standard);
        arpEnableBtn.onClick = [this] {
            bool on = arpEnableBtn.getToggleState();
            arpeggiator.enabled.store(on);
            arpEnableBtn.setButtonText(on ? "ON" : "OFF");
            Fanfare::setButtonRole(arpEnableBtn, on ? Fanfare::ButtonRole::Primary : Fanfare::ButtonRole::Standard);
            arpEnableBtn.repaint();
        };
        addAndMakeVisible(arpEnableBtn);
        setupArpControls();
        setupOctControls();

        // Harmony routing target (links generated voices to another strip)
        styleEngraved(harmonyTargetLabel, "ROUTE HARMONY TO", 12.5f);
        harmonyTargetLabel.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(harmonyTargetLabel);

        harmonyTargetCombo.addItem("None (Local)", 1); // id 1 == target -1
        for (int s = 0; s < slotNames.size(); ++s) {
            if (s == ownSlotIndex)
                continue; // never route a slot to itself
            harmonyTargetCombo.addItem(
                slotNames[s] + "  (Strip " + juce::String(s + 1) + ")", s + 2);
        }
        int desiredId = harmonizer.harmonyTargetSlot.load() + 2;
        harmonyTargetCombo.setSelectedId(
            (harmonyTargetCombo.indexOfItemId(desiredId) >= 0) ? desiredId : 1,
            juce::dontSendNotification);
        harmonyTargetCombo.onChange = [this] {
            harmonizer.harmonyTargetSlot.store(harmonyTargetCombo.getSelectedId() - 2);
            harmonizer.needsFlush.store(true);
        };
        addAndMakeVisible(harmonyTargetCombo);

        closeBtn.setButtonText("CLOSE");
        Fanfare::setButtonRole(closeBtn, Fanfare::ButtonRole::Danger);
        closeBtn.onClick = [this] { if (closeCallback) closeCallback(); };
        addAndMakeVisible(closeBtn);
        Fanfare::styleHelpButton(helpBtn, "#midi-effects", "HAAALP! Open the MIDI Effects & Harmonizer section of the User's Manual");
        addAndMakeVisible(helpBtn);
        setSize(620, 410);
    }

    void labelTextChanged(juce::Label* l) override {
        if (l == &arpBpmEditor) {
            float val = arpBpmEditor.getText().getFloatValue();
            if (val >= 1.0f && val <= 999.0f) arpeggiator.bpm.store(val);
            arpBpmEditor.setText(juce::String(arpeggiator.bpm.load(), 1), juce::dontSendNotification);
        }
    }

private:
    void setupArpControls() {
        auto styleLabel = [](juce::Label& lbl, const juce::String& t) {
            lbl.setText(t, juce::dontSendNotification);
            lbl.setFont(juce::FontOptions("Barlow Condensed", 12.0f, juce::Font::bold));
            lbl.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::engrave));
        };

        styleLabel(arpBpmLabel, "BPM");
        addAndMakeVisible(arpBpmLabel);

        arpBpmEditor.setText(juce::String(arpeggiator.bpm.load(), 1), juce::dontSendNotification);
        arpBpmEditor.setEditable(true); arpBpmEditor.addListener(this);
        arpBpmEditor.setJustificationType(juce::Justification::centred);
        arpBpmEditor.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 18.0f, juce::Font::bold));
        arpBpmEditor.setColour(juce::Label::backgroundColourId, ThemeManager::get(Theme::Role::sunken));
        arpBpmEditor.setColour(juce::Label::outlineColourId, ThemeManager::get(Theme::Role::border));
        arpBpmEditor.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::foh));

        arpBpmDownBtn.onClick = [this] {
            float v = juce::jlimit(1.0f, 999.0f, arpeggiator.bpm.load() - 1.0f);
            arpeggiator.bpm.store(v);
            arpBpmEditor.setText(juce::String(v, 1), juce::dontSendNotification);
        };
        arpBpmUpBtn.onClick = [this] {
            float v = juce::jlimit(1.0f, 999.0f, arpeggiator.bpm.load() + 1.0f);
            arpeggiator.bpm.store(v);
            arpBpmEditor.setText(juce::String(v, 1), juce::dontSendNotification);
        };
        addAndMakeVisible(arpBpmEditor);
        addAndMakeVisible(arpBpmDownBtn);
        addAndMakeVisible(arpBpmUpBtn);

        styleLabel(arpOctUpLabel, "OCT UP"); addAndMakeVisible(arpOctUpLabel);
        arpOctUpSlider.setRange(0, 4, 1);
        arpOctUpSlider.setValue(arpeggiator.octavesUp.load(), juce::dontSendNotification);
        arpOctUpSlider.onValueChange = [this] { arpeggiator.octavesUp.store((int)arpOctUpSlider.getValue()); };
        addAndMakeVisible(arpOctUpSlider);

        styleLabel(arpOctDownLabel, "OCT DN"); addAndMakeVisible(arpOctDownLabel);
        arpOctDownSlider.setRange(0, 4, 1);
        arpOctDownSlider.setValue(arpeggiator.octavesDown.load(), juce::dontSendNotification);
        arpOctDownSlider.onValueChange = [this] { arpeggiator.octavesDown.store((int)arpOctDownSlider.getValue()); };
        addAndMakeVisible(arpOctDownSlider);

        styleLabel(arpGateLabel, "GATE"); addAndMakeVisible(arpGateLabel);
        arpGateSlider.setRange(0.01, 1.0, 0.01);
        arpGateSlider.setValue(arpeggiator.gate.load(), juce::dontSendNotification);
        arpGateSlider.onValueChange = [this] { arpeggiator.gate.store((float)arpGateSlider.getValue()); };
        addAndMakeVisible(arpGateSlider);

        styleLabel(arpPatternLabel, "PATTERN"); addAndMakeVisible(arpPatternLabel);
        const char* names[] = {"UP", "DOWN", "U/D", "RND"};
        for (int i = 0; i < 4; ++i) {
            auto* btn = new juce::TextButton(names[i]);
            btn->setClickingTogglesState(true); btn->setRadioGroupId(1);
            bool active = (i == arpeggiator.patternIdx.load());
            btn->setToggleState(active, juce::dontSendNotification);
            Fanfare::setButtonRole(*btn, active ? Fanfare::ButtonRole::Primary : Fanfare::ButtonRole::Standard);
            int idx = i;
            btn->onClick = [this, idx] {
                arpeggiator.patternIdx.store(idx);
                for (int b = 0; b < arpPatternBtns.size(); ++b) {
                    Fanfare::setButtonRole(*arpPatternBtns[b], (b == idx) ? Fanfare::ButtonRole::Primary : Fanfare::ButtonRole::Standard);
                    arpPatternBtns[b]->repaint();
                }
            };
            arpPatternBtns.add(btn); addAndMakeVisible(btn);
        }
    }

    void setupOctControls() {
        auto styleLabel = [](juce::Label& lbl, const juce::String& t) {
            lbl.setText(t, juce::dontSendNotification);
            lbl.setFont(juce::FontOptions("Barlow Condensed", 12.0f, juce::Font::bold));
            lbl.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::engrave));
        };

        octHeader.setText("OCTAVER & HARMONIZER", juce::dontSendNotification);
        octHeader.setFont(juce::FontOptions("Barlow Condensed", 14.0f, juce::Font::bold));
        octHeader.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::iem));
        addAndMakeVisible(octHeader);

        octEnableBtn.setClickingTogglesState(true);
        octEnableBtn.setToggleState(harmonizer.enabled.load(), juce::dontSendNotification);
        octEnableBtn.setButtonText(octEnableBtn.getToggleState() ? "ON" : "OFF");
        Fanfare::setButtonRole(octEnableBtn, octEnableBtn.getToggleState() ? Fanfare::ButtonRole::Primary : Fanfare::ButtonRole::Standard);
        octEnableBtn.onClick = [this] {
            bool on = octEnableBtn.getToggleState();
            harmonizer.enabled.store(on);
            harmonizer.needsFlush.store(true);
            octEnableBtn.setButtonText(on ? "ON" : "OFF");
            Fanfare::setButtonRole(octEnableBtn, on ? Fanfare::ButtonRole::Primary : Fanfare::ButtonRole::Standard);
            octEnableBtn.repaint();
        };
        addAndMakeVisible(octEnableBtn);

        styleLabel(octUpLabel, "OCTAVES UP"); addAndMakeVisible(octUpLabel);
        octUpSlider.setRange(0, 3, 1);
        octUpSlider.setValue(harmonizer.octavesUp.load(), juce::dontSendNotification);
        octUpSlider.onValueChange = [this] { harmonizer.octavesUp.store((int)octUpSlider.getValue()); harmonizer.needsFlush.store(true); };
        addAndMakeVisible(octUpSlider);

        styleLabel(octDownLabel, "OCTAVES DOWN"); addAndMakeVisible(octDownLabel);
        octDownSlider.setRange(0, 3, 1);
        octDownSlider.setValue(harmonizer.octavesDown.load(), juce::dontSendNotification);
        octDownSlider.onValueChange = [this] { harmonizer.octavesDown.store((int)octDownSlider.getValue()); harmonizer.needsFlush.store(true); };
        addAndMakeVisible(octDownSlider);

        addAndMakeVisible(africaModeBtn);
        int initialMode = harmonizer.africaMode.load();
        auto getBtnText = [](int mode) -> juce::String {
            if (mode == 1) return "AFRICA: PART 1";
            if (mode == 2) return "AFRICA: PART 2";
            if (mode == 3) return "PLAY IN C SPLIT";
            return "AFRICA HARM: OFF";
        };
        africaModeBtn.setButtonText(getBtnText(initialMode));
        africaModeBtn.setClickingTogglesState(false);
        Fanfare::setButtonRole(africaModeBtn, initialMode > 0 ? Fanfare::ButtonRole::Primary : Fanfare::ButtonRole::Standard);
        africaModeBtn.onClick = [this, getBtnText] {
            int current = harmonizer.africaMode.load();
            int next = (current + 1) % 4;
            harmonizer.africaMode.store(next); harmonizer.needsFlush.store(true);

            africaModeBtn.setButtonText(getBtnText(next));
            Fanfare::setButtonRole(africaModeBtn, next > 0 ? Fanfare::ButtonRole::Primary : Fanfare::ButtonRole::Standard);
            africaModeBtn.repaint();
        };

        octInfo.setText("Layers octave notes on top of what you play.\nReference-counted: no stuck notes if you\nplay the same note the software generates.", juce::dontSendNotification);
        octInfo.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::textDim));
        octInfo.setFont(juce::FontOptions(11.5f));
        addAndMakeVisible(octInfo);
    }

public:
    void paint(juce::Graphics& g) override {
        Fanfare::ModalFrame::paintOverlayScrimAndPanel(
            g, getLocalBounds(), getLocalBounds(),
            "MIDI EFFECTS // " + slotRef.getName().toUpperCase(), 44);

        // Divider between Transpose row and Arp/Octaver columns
        g.setColour(ThemeManager::get(Theme::Role::hairline));
        g.drawHorizontalLine(92, 12.0f, (float)getWidth() - 12.0f);
        g.drawVerticalLine(getWidth() / 2, 98.0f, (float)getHeight() - 12.0f);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(12);
        auto top = area.removeFromTop(30);
        closeBtn.setBounds(top.removeFromRight(56).reduced(0, 2));
        top.removeFromRight(6);
        helpBtn.setBounds(top.removeFromRight(74).reduced(0, 2));

        area.removeFromTop(8);
        auto transRow = area.removeFromTop(40);
        transposeHeader.setBounds(transRow.removeFromLeft(95));
        transRow.removeFromLeft(6);
        transposeUpBtn.setBounds(transRow.removeFromRight(24).reduced(0, 6));
        transRow.removeFromRight(2);
        transposeDownBtn.setBounds(transRow.removeFromRight(24).reduced(0, 6));
        transRow.removeFromRight(6);
        transposeValue.setBounds(transRow.removeFromRight(60));
        transposeSlider.setBounds(transRow.reduced(6, 8));
        area.removeFromTop(8);

        auto left = area.removeFromLeft(area.getWidth() / 2).reduced(6);
        auto right = area.reduced(6);

        auto arpRow = left.removeFromTop(28);
        arpHeader.setBounds(arpRow.removeFromLeft(arpRow.getWidth() - 60));
        arpEnableBtn.setBounds(arpRow.removeFromRight(50));
        left.removeFromTop(6);

        auto bpmRow = left.removeFromTop(46);
        arpBpmLabel.setBounds(bpmRow.removeFromLeft(35));
        arpBpmEditor.setBounds(bpmRow.removeFromLeft(100).reduced(2, 6));
        bpmRow.removeFromLeft(4);
        arpBpmDownBtn.setBounds(bpmRow.removeFromLeft(28).reduced(2, 10));
        arpBpmUpBtn.setBounds(bpmRow.removeFromLeft(28).reduced(2, 10));
        left.removeFromTop(6);

        auto knobRow = left.removeFromTop(70);
        int kw = knobRow.getWidth() / 3;
        auto k1 = knobRow.removeFromLeft(kw).reduced(3);
        arpOctUpLabel.setBounds(k1.removeFromTop(16)); arpOctUpSlider.setBounds(k1);
        auto k2 = knobRow.removeFromLeft(kw).reduced(3);
        arpOctDownLabel.setBounds(k2.removeFromTop(16)); arpOctDownSlider.setBounds(k2);
        auto k3 = knobRow.reduced(3);
        arpGateLabel.setBounds(k3.removeFromTop(16)); arpGateSlider.setBounds(k3);
        left.removeFromTop(6);
        arpPatternLabel.setBounds(left.removeFromTop(16));
        auto patRow = left.removeFromTop(28);
        int pw = patRow.getWidth() / 4;
        for (int i = 0; i < arpPatternBtns.size(); ++i)
            arpPatternBtns[i]->setBounds(patRow.removeFromLeft(pw).reduced(2));

        auto octRow = right.removeFromTop(28);
        octHeader.setBounds(octRow.removeFromLeft(octRow.getWidth() - 60));
        octEnableBtn.setBounds(octRow.removeFromRight(50));
        right.removeFromTop(6);
        auto octKnobRow = right.removeFromTop(80);
        int okw = octKnobRow.getWidth() / 2;
        auto ok1 = octKnobRow.removeFromLeft(okw).reduced(6);
        octUpLabel.setBounds(ok1.removeFromTop(18)); octUpSlider.setBounds(ok1);
        auto ok2 = octKnobRow.reduced(6);
        octDownLabel.setBounds(ok2.removeFromTop(18)); octDownSlider.setBounds(ok2);
        right.removeFromTop(4);

        auto harmRow = right.removeFromTop(24);
        africaModeBtn.setBounds(harmRow.reduced(10, 0));
        right.removeFromTop(4);

        harmonyTargetLabel.setBounds(right.removeFromTop(16));
        harmonyTargetCombo.setBounds(right.removeFromTop(24).reduced(2, 0));
        right.removeFromTop(4);

        octInfo.setBounds(right);
    }

private:
    SimpleArpeggiator& arpeggiator;
    OctaveHarmonizer& harmonizer;
    RackSlot& slotRef;
    std::function<void()> closeCallback;

    juce::TextButton closeBtn{"CLOSE"};
    juce::TextButton helpBtn{"HAAALP!"};

    juce::Label transposeHeader, transposeValue;
    juce::Slider transposeSlider;
    juce::ArrowButton transposeDownBtn;
    juce::ArrowButton transposeUpBtn;

    juce::Label arpHeader, arpBpmLabel, arpOctUpLabel, arpOctDownLabel, arpGateLabel, arpPatternLabel;
    juce::Label arpBpmEditor;
    juce::ArrowButton arpBpmDownBtn;
    juce::ArrowButton arpBpmUpBtn;
    juce::TextButton arpEnableBtn{"ON"};
    juce::Slider arpOctUpSlider, arpOctDownSlider, arpGateSlider;
    juce::OwnedArray<juce::TextButton> arpPatternBtns;

    juce::Label octHeader, octUpLabel, octDownLabel, octInfo;
    juce::TextButton octEnableBtn{"ON"};
    juce::Slider octUpSlider, octDownSlider;
    juce::TextButton africaModeBtn{"AFRICA HARM"};
    juce::Label harmonyTargetLabel;
    juce::ComboBox harmonyTargetCombo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiEffectsComponent)
};
