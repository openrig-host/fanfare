#pragma once

#include <JuceHeader.h>
#include "SetlistManager.h"
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "ThemeManager.h"

namespace Fanfare {

class SetlistPanel : public juce::Component,
                     public juce::ListBoxModel,
                     public juce::DragAndDropTarget,
                     public juce::ChangeListener {
public:
    SetlistPanel() {
        listBox.setModel(this);
        listBox.setRowHeight(24);
        listBox.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible(listBox);

        addBtn.setButtonText("+ SONG");
        setButtonRole(addBtn, ButtonRole::Primary);
        addBtn.onClick = [this] {
            if (onAddCurrentRequested)
                onAddCurrentRequested();
        };
        addAndMakeVisible(addBtn);

        removeBtn.setButtonText("REMOVE");
        setButtonRole(removeBtn, ButtonRole::Danger);
        removeBtn.onClick = [this] {
            int sel = listBox.getSelectedRow();
            if (sel >= 0) {
                SetlistManager::getInstance().removeSetup(sel);
            }
        };
        addAndMakeVisible(removeBtn);

        moveUpBtn.setButtonText("UP");
        setButtonRole(moveUpBtn, ButtonRole::Standard);
        moveUpBtn.onClick = [this] {
            int sel = listBox.getSelectedRow();
            if (sel > 0) {
                SetlistManager::getInstance().moveSetupUp(sel);
                listBox.selectRow(sel - 1);
            }
        };
        addAndMakeVisible(moveUpBtn);

        moveDownBtn.setButtonText("DN");
        setButtonRole(moveDownBtn, ButtonRole::Standard);
        moveDownBtn.onClick = [this] {
            int sel = listBox.getSelectedRow();
            if (sel >= 0 && sel < SetlistManager::getInstance().getSetups().size() - 1) {
                SetlistManager::getInstance().moveSetupDown(sel);
                listBox.selectRow(sel + 1);
            }
        };
        addAndMakeVisible(moveDownBtn);

        saveBtn.setButtonText("SAVE");
        setButtonRole(saveBtn, ButtonRole::Standard);
        saveBtn.onClick = [this] {
            fileChooser = std::make_unique<juce::FileChooser>(
                "Save Setlist",
                RigLibrary::getSongsDirectory(),
                "*.orsetlist");
            fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& fc) {
                    auto file = fc.getResult();
                    if (file != juce::File{}) {
                        if (file.getFileExtension().isEmpty())
                            file = file.withFileExtension(".orsetlist");
                        SetlistManager::getInstance().saveSetlist(file);
                    }
                });
        };
        addAndMakeVisible(saveBtn);

        loadBtn.setButtonText("LOAD");
        setButtonRole(loadBtn, ButtonRole::Standard);
        loadBtn.onClick = [this] {
            fileChooser = std::make_unique<juce::FileChooser>(
                "Load Setlist",
                RigLibrary::getSongsDirectory(),
                "*.orsetlist");
            fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& fc) {
                    auto file = fc.getResult();
                    if (file.existsAsFile()) {
                        SetlistManager::getInstance().loadSetlist(file);
                    }
                });
        };
        addAndMakeVisible(loadBtn);

        clearBtn.setButtonText("CLEAR");
        setButtonRole(clearBtn, ButtonRole::Danger);
        clearBtn.onClick = [this] {
            SetlistManager::getInstance().clearSetlist();
        };
        addAndMakeVisible(clearBtn);

        SetlistManager::getInstance().addChangeListener(this);
    }

    ~SetlistPanel() override {
        SetlistManager::getInstance().removeChangeListener(this);
    }

    std::function<void()> onAddCurrentRequested;
    std::function<void(const juce::File&, int)> onLoadSetupRequested;

    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        listBox.updateContent();
        listBox.repaint();
    }

    int getNumRows() override {
        return SetlistManager::getInstance().getSetups().size();
    }

    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool isSelected) override {
        auto setups = SetlistManager::getInstance().getSetups();
        if (row < 0 || row >= setups.size())
            return;

        const auto& t = ThemeManager::getInstance().active();
        int activeIdx = SetlistManager::getInstance().getActiveIndex();
        const bool isLive = (row == activeIdx);

        if (isLive) {
            g.fillAll(t.foh.withAlpha(t.dark ? 0.18f : 0.14f));
            g.setColour(t.foh);
            g.fillRect(0.0f, 0.0f, 3.0f, (float)height);
        } else if (isSelected) {
            g.fillAll(t.accent.withAlpha(t.dark ? 0.22f : 0.15f));
            g.setColour(t.accent);
            g.fillRect(0.0f, 0.0f, 2.5f, (float)height);
        } else if (row % 2 == 1) {
            g.fillAll(t.text.withAlpha(0.02f));
        }

        // Row number
        g.setFont(BoutiqueLookAndFeel::monoFont(10.5f, true));
        g.setColour(isLive ? t.foh : t.engrave);
        g.drawText(juce::String(row + 1).paddedLeft('0', 2),
                   6, 0, 20, height, juce::Justification::centredLeft, false);

        // Song title
        juce::String name = setups[row].getFileNameWithoutExtension();
        g.setFont(BoutiqueLookAndFeel::uiFont(13.0f, isLive || isSelected));
        g.setColour(isLive ? t.foh : t.text);
        g.drawText(name, 28, 0, width - (isLive ? 68 : 34), height,
                   juce::Justification::centredLeft, true);

        if (isLive) {
            g.setFont(BoutiqueLookAndFeel::condensedFont(10.0f, 0.08f));
            g.setColour(t.foh);
            g.drawText("LIVE", width - 38, 0, 32, height, juce::Justification::centredRight, false);
        }
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override {
        auto setups = SetlistManager::getInstance().getSetups();
        if (row >= 0 && row < setups.size()) {
            if (onLoadSetupRequested)
                onLoadSetupRequested(setups[row], row);
        }
    }

    bool isInterestedInDragSource(const SourceDetails& details) override {
        juce::String desc = details.description.toString();
        return desc.endsWithIgnoreCase(".json");
    }

    void itemDropped(const SourceDetails& details) override {
        juce::File file(details.description.toString());
        if (file.existsAsFile()) {
            SetlistManager::getInstance().addSetup(file);
        }
    }

    void resized() override {
        auto b = getLocalBounds().reduced(4);

        auto controlRow = b.removeFromBottom(26);
        int w1 = controlRow.getWidth();
        addBtn.setBounds(controlRow.removeFromLeft((w1 * 32) / 100).reduced(1));
        removeBtn.setBounds(controlRow.removeFromLeft((w1 * 30) / 100).reduced(1));
        moveUpBtn.setBounds(controlRow.removeFromLeft((w1 * 19) / 100).reduced(1));
        moveDownBtn.setBounds(controlRow.reduced(1));

        b.removeFromBottom(2);
        auto fileRow = b.removeFromBottom(26);
        int w2 = fileRow.getWidth() / 3;
        loadBtn.setBounds(fileRow.removeFromLeft(w2).reduced(1));
        saveBtn.setBounds(fileRow.removeFromLeft(w2).reduced(1));
        clearBtn.setBounds(fileRow.reduced(1));

        b.removeFromBottom(4);
        listBox.setBounds(b);
    }

private:
    juce::ListBox listBox;
    juce::TextButton addBtn, removeBtn, moveUpBtn, moveDownBtn;
    juce::TextButton loadBtn, saveBtn, clearBtn;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SetlistPanel)
};

} // namespace Fanfare
