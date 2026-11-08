#pragma once

#include <JuceHeader.h>
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "SaveHistory.h"
#include "ThemeManager.h"

namespace Fanfare {

// VersionHistoryModal - inspect and restore timestamped snapshots of any song,
// strip preset, or queue map. Restoring is append-only (snapshots current state
// first, restores chosen snapshot, and records it).
class VersionHistoryModal : public juce::Component, private juce::ListBoxModel {
public:
    std::function<void()> onClose;
    std::function<void(const juce::File&)> onRestore;

    explicit VersionHistoryModal(const juce::File& target) : item(target) {
        addAndMakeVisible(subLabel);
        subLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::textDim));
        subLabel.setFont(BoutiqueLookAndFeel::uiFont(12.0f, false));

        addAndMakeVisible(list);
        list.setModel(this);
        list.setColour(juce::ListBox::backgroundColourId, ThemeManager::get(Theme::Role::sunken));
        list.setColour(juce::ListBox::outlineColourId, ThemeManager::get(Theme::Role::border));
        list.setOutlineThickness(1);
        list.setRowHeight(30);

        restoreBtn.setButtonText("Restore selected");
        setButtonRole(restoreBtn, ButtonRole::Primary);
        restoreBtn.onClick = [this] { confirmAndRestoreSelected(); };
        addAndMakeVisible(restoreBtn);

        revealBtn.setButtonText("Open history folder");
        setButtonRole(revealBtn, ButtonRole::Subtle);
        revealBtn.onClick = [this] {
            auto dir = SaveHistory::dirFor(item);
            dir.createDirectory();
            dir.revealToUser();
        };
        addAndMakeVisible(revealBtn);

        closeBtn.setButtonText("Close");
        setButtonRole(closeBtn, ButtonRole::Standard);
        closeBtn.onClick = [this] { close(); };
        addAndMakeVisible(closeBtn);

        refresh();
    }

    void refresh() {
        files = SaveHistory::versions(item);
        list.updateContent();
        if (!files.isEmpty())
            list.selectRow(0);
        restoreBtn.setEnabled(!files.isEmpty());

        const juce::String dot = juce::String::charToString(0x00B7);
        subLabel.setText(juce::String(files.size()) + " saved versions  " + dot +
                             "  keeps newest 20 + past 7 days  " + dot +
                             "  restore never deletes history",
                         juce::dontSendNotification);
    }

    void paint(juce::Graphics& g) override {
        const bool inDialog = (findParentComponentOfClass<juce::DialogWindow>() != nullptr);
        ModalFrame::paintOverlayScrimAndPanel(
            g, getLocalBounds(), panelBounds(),
            "Version history - " + item.getFileNameWithoutExtension(), 42, !inDialog);

        if (files.isEmpty()) {
            g.setColour(ThemeManager::get(Theme::Role::textFaint));
            g.setFont(BoutiqueLookAndFeel::uiFont(13.0f, false));
            g.drawText("No snapshots recorded yet (saved automatically whenever this item changes).",
                       list.getBounds(), juce::Justification::centred, true);
        }
    }

    void resized() override {
        auto b = panelBounds().reduced(16);
        b.removeFromTop(38);
        subLabel.setBounds(b.removeFromTop(20));
        b.removeFromTop(8);

        auto buttons = b.removeFromBottom(30);
        b.removeFromBottom(10);

        restoreBtn.setBounds(buttons.removeFromLeft(140));
        buttons.removeFromLeft(8);
        revealBtn.setBounds(buttons.removeFromLeft(154));
        closeBtn.setBounds(buttons.removeFromRight(88));

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
        return getLocalBounds().withSizeKeepingCentre(
            juce::jmin(640, getWidth() - 40),
            juce::jmin(440, getHeight() - 40));
    }

    void close() {
        if (onClose)
            onClose();
        else if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(0);
    }

    int getNumRows() override { return files.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool selected) override {
        if (row < 0 || row >= files.size())
            return;
        const auto& t = ThemeManager::getInstance().active();
        const auto f = files.getReference(row);

        if (selected) {
            g.fillAll(t.accent.withAlpha(0.20f));
            g.setColour(t.accent);
            g.fillRect(0, 3, 2, height - 6);
        } else if (row % 2 == 1) {
            g.fillAll(t.panelAlt.withAlpha(0.45f));
        }

        g.setColour(t.text);
        g.setFont(BoutiqueLookAndFeel::monoFont(12.5f, row == 0));
        g.drawText(SaveHistory::describeVersion(f), 12, 0, 190, height,
                   juce::Justification::centredLeft, false);

        const auto snapTime = SaveHistory::parseVersionTime(f);
        const auto age = juce::RelativeTime::milliseconds(
            juce::jmax<juce::int64>(0, (juce::Time::getCurrentTime() - snapTime).inMilliseconds()));

        g.setColour(row == 0 ? t.foh : t.textDim);
        g.setFont(BoutiqueLookAndFeel::uiFont(12.0f, row == 0));
        const juce::String ageStr = (row == 0 ? "Latest  (" + humanAge(age) + ")" : humanAge(age));
        g.drawText(ageStr, 210, 0, 180, height, juce::Justification::centredLeft, true);

        g.setColour(t.textFaint);
        g.setFont(BoutiqueLookAndFeel::monoFont(11.5f, false));
        const auto kb = juce::jmax<juce::int64>(1, (f.getSize() + 512) / 1024);
        g.drawText(juce::String(kb) + " KB", width - 92, 0, 80, height,
                   juce::Justification::centredRight, false);
    }

    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override {
        confirmAndRestoreSelected();
    }

    static juce::String humanAge(const juce::RelativeTime& t) {
        const int mins = (int)t.inMinutes();
        if (mins < 1)
            return "just now";
        if (mins < 60)
            return juce::String(mins) + " min ago";
        const int hrs = (int)t.inHours();
        if (hrs < 24)
            return juce::String(hrs) + " h ago";
        return juce::String((int)t.inDays()) + " days ago";
    }

    void confirmAndRestoreSelected() {
        const int row = list.getSelectedRow();
        if (row < 0 || row >= files.size())
            return;
        const auto versionFile = files.getReference(row);
        const auto desc = SaveHistory::describeVersion(versionFile);

        juce::Component::SafePointer<VersionHistoryModal> safeThis(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::QuestionIcon)
                .withTitle("Restore version")
                .withMessage("Restore \"" + item.getFileNameWithoutExtension() +
                             "\" to snapshot from " + desc +
                             "?\n\nThe current saved file on disk is backed up in history first. "
                             "Any unsaved in-memory mixer changes will be replaced when the restored version loads.")
                .withButton("Restore")
                .withButton("Cancel"),
            [safeThis, versionFile](int result) {
                if (result != 1 || safeThis == nullptr)
                    return;
                juce::String restoreErr;
                if (!SaveHistory::restoreVersion(versionFile, safeThis->item, restoreErr)) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        "Restore failed",
                        restoreErr);
                    return;
                }

                auto cb = safeThis->onRestore;
                const auto restoredItem = safeThis->item;
                safeThis->close();
                if (cb)
                    cb(restoredItem);
            });
    }

    juce::File item;
    juce::Array<juce::File> files;
    juce::ListBox list;
    juce::Label subLabel;
    juce::TextButton restoreBtn, revealBtn, closeBtn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VersionHistoryModal)
};

inline void showVersionHistoryDialog(const juce::File& file,
                                     std::function<void(const juce::File&)> onRestored = {}) {
    juce::DialogWindow::LaunchOptions o;
    auto* modal = new VersionHistoryModal(file);
    modal->onRestore = std::move(onRestored);
    modal->setSize(620, 420);
    o.content.setOwned(modal);
    o.dialogTitle = "Version history - " + file.getFileNameWithoutExtension();
    o.dialogBackgroundColour = ThemeManager::get(Theme::Role::panel);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

} // namespace Fanfare

