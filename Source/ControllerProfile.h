#pragma once

#include <JuceHeader.h>
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "FanfareConstants.h"
#include "MidiLearnBus.h"
#include "ThemeManager.h"

// ControllerProfile - "define your controller once".
//
// The player names the physical knobs / faders / pedals on their keyboard and
// the CC each one sends (type it or press Learn and move the control). Fanfare
// then uses that map everywhere: one click in a strip's CC manager allows the
// whole controller on that strip, and CC numbers are shown by control name
// ("Drawbar 8' (CC79)") instead of bare numbers.
//
// Replaces the hard-wired CK88 preset button: the default profile is seeded
// with the same Roland CK88 CCs, so nothing changes until the profile is edited.
// Stored in %APPDATA%/Fanfare/settings/controller_profile.json.
namespace Fanfare {

struct ControllerControl {
    juce::String name;
    int cc = -1;
};

class ControllerProfile : public juce::ChangeBroadcaster {
public:
    static ControllerProfile& get() {
        static ControllerProfile instance;
        return instance;
    }

    juce::String name;
    std::vector<ControllerControl> controls;

    static juce::File file() {
        return FanfareConstants::getAppDirectory().getChildFile("settings")
            .getChildFile("controller_profile.json");
    }

    void seedDefaults() {
        name = "Roland CK88";
        controls = {
            {"Mod wheel", 9}, {"Sustain pedal", 64}, {"Chorus / delay knob", 93},
            {"Reverb knob", 92}, {"Drawbar 16'", 77}, {"Drawbar 5-1/3'", 78},
            {"Drawbar 8'", 79}, {"Drawbar 4'", 80}, {"Drawbar 2-2/3'", 81},
            {"Drawbar 2'", 82}, {"Drawbar 1-3/5'", 83}, {"Drawbar 1-1/3'", 85},
            {"Drawbar 1'", 86},
        };
    }

    void load() {
        auto f = file();
        if (!f.existsAsFile()) {
            seedDefaults();
            return;
        }
        auto parsed = juce::JSON::parse(f);
        auto* obj = parsed.getDynamicObject();
        if (obj == nullptr) {
            seedDefaults();
            return;
        }
        name = obj->getProperty("name").toString();
        controls.clear();
        if (auto* arr = obj->getProperty("controls").getArray()) {
            for (auto& v : *arr) {
                if (auto* c = v.getDynamicObject()) {
                    ControllerControl cc;
                    cc.name = c->getProperty("name").toString();
                    cc.cc = (int)c->getProperty("cc");
                    controls.push_back(cc);
                }
            }
        }
    }

    void save() {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("name", name);
        juce::Array<juce::var> arr;
        for (const auto& c : controls) {
            juce::DynamicObject::Ptr co = new juce::DynamicObject();
            co->setProperty("name", c.name);
            co->setProperty("cc", c.cc);
            arr.add(juce::var(co.get()));
        }
        obj->setProperty("controls", arr);
        auto f = file();
        f.getParentDirectory().createDirectory();
        f.replaceWithText(juce::JSON::toString(juce::var(obj.get())));
        sendChangeMessage();
    }

    // CCs that are actually assigned (0..127), de-duplicated
    std::vector<int> assignedCCs() const {
        std::vector<int> out;
        for (const auto& c : controls)
            if (c.cc >= 0 && c.cc <= 127 &&
                std::find(out.begin(), out.end(), c.cc) == out.end())
                out.push_back(c.cc);
        return out;
    }

    // "Drawbar 8' (CC79)" if the CC is in the profile, else "CC79"
    juce::String describeCC(int cc) const {
        for (const auto& c : controls)
            if (c.cc == cc && c.name.isNotEmpty())
                return c.name + " (CC" + juce::String(cc) + ")";
        return "CC" + juce::String(cc);
    }

private:
    ControllerProfile() { load(); }
};

//==============================================================================
// Editor overlay: name the controller, list its controls, learn each CC.
class ControllerProfileEditor : public juce::Component, private juce::ListBoxModel {
public:
    std::function<void()> onClose;

    ControllerProfileEditor() {
        auto& p = ControllerProfile::get();
        working = p.controls;

        addAndMakeVisible(nameEditor);
        nameEditor.setText(p.name, false);
        nameEditor.setTextToShowWhenEmpty("Controller name (e.g. Roland CK88)",
                                          ThemeManager::get(Theme::Role::textFaint));

        addAndMakeVisible(hintLabel);
        hintLabel.setText("Name each knob, fader or pedal and the CC it sends. Press Learn, "
                          "then move the control.",
                          juce::dontSendNotification);
        hintLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::textDim));
        hintLabel.setFont(BoutiqueLookAndFeel::uiFont(12.5f));

        addAndMakeVisible(list);
        list.setModel(this);
        list.setRowHeight(30);
        list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);

        addBtn.setButtonText("+ Add control");
        setButtonRole(addBtn, ButtonRole::Subtle);
        addBtn.onClick = [this] {
            working.push_back({"Control " + juce::String((int)working.size() + 1), -1});
            list.updateContent();
            list.scrollToEnsureRowIsOnscreen((int)working.size() - 1);
        };
        addAndMakeVisible(addBtn);

        saveBtn.setButtonText("Save");
        setButtonRole(saveBtn, ButtonRole::Primary);
        saveBtn.setToggleState(true, juce::dontSendNotification);
        saveBtn.onClick = [this] {
            auto& prof = ControllerProfile::get();
            prof.name = nameEditor.getText().trim();
            prof.controls = working;
            prof.save();
            close();
        };
        addAndMakeVisible(saveBtn);

        cancelBtn.setButtonText("Cancel");
        setButtonRole(cancelBtn, ButtonRole::Subtle);
        cancelBtn.onClick = [this] { close(); };
        addAndMakeVisible(cancelBtn);
    }

    ~ControllerProfileEditor() override {
        MidiLearnBus::getInstance().disarmIfOwner(this);
    }

    void paint(juce::Graphics& g) override {
        ModalFrame::paintOverlayScrimAndPanel(g, getLocalBounds(), panelBounds(),
                                              "Controller profile", 44);
    }

    void resized() override {
        auto b = panelBounds().reduced(16);
        b.removeFromTop(40);
        nameEditor.setBounds(b.removeFromTop(28));
        b.removeFromTop(6);
        hintLabel.setBounds(b.removeFromTop(20));
        b.removeFromTop(6);
        auto bottom = b.removeFromBottom(30);
        saveBtn.setBounds(bottom.removeFromRight(90));
        bottom.removeFromRight(8);
        cancelBtn.setBounds(bottom.removeFromRight(90));
        addBtn.setBounds(bottom.removeFromLeft(120));
        b.removeFromBottom(8);
        list.setBounds(b);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (!panelBounds().contains(e.getPosition()))
            close();
    }

private:
    juce::Rectangle<int> panelBounds() const {
        if (findParentComponentOfClass<juce::DialogWindow>() != nullptr)
            return getLocalBounds();
        return getLocalBounds().withSizeKeepingCentre(juce::jmin(560, getWidth() - 40),
                                                      juce::jmin(560, getHeight() - 40));
    }

    void close() {
        MidiLearnBus::getInstance().disarmIfOwner(this);
        if (onClose)
            onClose();
        else if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(0);
    }

    // One editable row per control
    struct Row : public juce::Component {
        ControllerProfileEditor& owner;
        int index = -1;
        juce::TextEditor nameEd, ccEd;
        juce::TextButton learnBtn{"Learn"}, removeBtn{"x"};

        explicit Row(ControllerProfileEditor& o) : owner(o) {
            addAndMakeVisible(nameEd);
            addAndMakeVisible(ccEd);
            ccEd.setInputRestrictions(3, "0123456789");
            ccEd.setJustification(juce::Justification::centred);
            ccEd.setFont(BoutiqueLookAndFeel::monoFont(12.5f));
            addAndMakeVisible(learnBtn);
            addAndMakeVisible(removeBtn);
            setButtonRole(removeBtn, ButtonRole::Subtle);
            removeBtn.setTooltip("Remove this control");
            nameEd.onTextChange = [this] {
                if (valid()) owner.working[(size_t)index].name = nameEd.getText();
            };
            ccEd.onTextChange = [this] {
                if (valid()) owner.working[(size_t)index].cc =
                    ccEd.getText().isEmpty() ? -1 : juce::jlimit(0, 127, ccEd.getText().getIntValue());
            };
            learnBtn.onClick = [this] { owner.armLearn(index); };
            removeBtn.onClick = [this] { owner.removeControl(index); };
        }
        bool valid() const { return index >= 0 && index < (int)owner.working.size(); }
        void update(int newIndex) {
            index = newIndex;
            if (!valid()) { setVisible(false); return; }
            setVisible(true);
            const auto& c = owner.working[(size_t)index];
            nameEd.setText(c.name, juce::dontSendNotification);
            ccEd.setText(c.cc >= 0 ? juce::String(c.cc) : juce::String(), juce::dontSendNotification);
            const bool armed = owner.learningIndex == index;
            learnBtn.setToggleState(armed, juce::dontSendNotification);
            learnBtn.setButtonText(armed ? "Move it..." : "Learn");
        }
        void resized() override {
            auto b = getLocalBounds().reduced(2, 3);
            removeBtn.setBounds(b.removeFromRight(26));
            b.removeFromRight(6);
            learnBtn.setBounds(b.removeFromRight(84));
            b.removeFromRight(6);
            ccEd.setBounds(b.removeFromRight(56));
            b.removeFromRight(6);
            nameEd.setBounds(b);
        }
    };

    int getNumRows() override { return (int)working.size(); }
    void paintListBoxItem(int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow(int row, bool, juce::Component* existing) override {
        auto* r = dynamic_cast<Row*>(existing);
        if (r == nullptr) {
            delete existing;
            r = new Row(*this);
        }
        r->update(row);
        return r;
    }

    void armLearn(int index) {
        learningIndex = index;
        list.updateContent();
        list.repaint();
        juce::Component::SafePointer<ControllerProfileEditor> safeThis(this);
        MidiLearnBus::Target t;
        t.label = "Controller profile: " + (index < (int)working.size() ? working[(size_t)index].name : juce::String());
        t.onBind = [safeThis, index](int cc) {
            if (safeThis == nullptr) return;
            if (index >= 0 && index < (int)safeThis->working.size())
                safeThis->working[(size_t)index].cc = cc;
            safeThis->learningIndex = -1;
            safeThis->list.updateContent();
            safeThis->list.repaint();
        };
        MidiLearnBus::getInstance().arm(std::move(t), this);
    }

    void removeControl(int index) {
        if (index < 0 || index >= (int)working.size()) return;
        working.erase(working.begin() + index);
        learningIndex = -1;
        list.updateContent();
        list.repaint();
    }

    std::vector<ControllerControl> working;
    int learningIndex = -1;
    juce::TextEditor nameEditor;
    juce::Label hintLabel;
    juce::ListBox list;
    juce::TextButton addBtn, saveBtn, cancelBtn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ControllerProfileEditor)
};

// Opens the editor in its own dialog window (works from modals and callouts).
inline void showControllerProfileEditor() {
    juce::DialogWindow::LaunchOptions o;
    auto* ed = new ControllerProfileEditor();
    ed->setSize(560, 540);
    o.content.setOwned(ed);
    o.dialogTitle = "Controller profile";
    o.dialogBackgroundColour = ThemeManager::get(Theme::Role::panel);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

} // namespace Fanfare
