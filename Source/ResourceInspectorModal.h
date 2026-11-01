#pragma once

#include <JuceHeader.h>
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "FanfareEngine.h"
#include "HelpLauncher.h"
#include "ThemeManager.h"

class ResourceInspectorModal : public juce::Component,
                               public juce::TableListBoxModel,
                               public juce::Timer {
public:
    ResourceInspectorModal(FanfareEngine& eng, std::function<void()> onClose)
        : engine(eng), closeCallback(std::move(onClose))
    {
        setOpaque(false);
        titleLabel.setVisible(false);

        closeBtn.setButtonText("CLOSE");
        Fanfare::setButtonRole(closeBtn, Fanfare::ButtonRole::Subtle);
        closeBtn.onClick = [this] {
            if (closeCallback) closeCallback();
        };
        addAndMakeVisible(closeBtn);

        Fanfare::styleHelpButton(helpBtn, "#resource-inspector", "HAAALP! Open the Resource & DSP Inspector section of the User's Manual");
        addAndMakeVisible(helpBtn);

        refreshBtn.setButtonText("REFRESH STATS");
        Fanfare::setButtonRole(refreshBtn, Fanfare::ButtonRole::Standard);
        refreshBtn.onClick = [this] {
            updateData();
        };
        addAndMakeVisible(refreshBtn);

        infoLabel.setText("Note: In-process VST/VST3 memory is pooled into Host Process RAM. DSP CPU % is measured per-slot in real-time.", juce::dontSendNotification);
        infoLabel.setFont(BoutiqueLookAndFeel::uiFont(11.5f, false));
        infoLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::textDim));
        addAndMakeVisible(infoLabel);

        table.setModel(this);
        table.setColour(juce::ListBox::backgroundColourId, ThemeManager::get(Theme::Role::sunken));
        table.setColour(juce::ListBox::outlineColourId, ThemeManager::get(Theme::Role::border));
        table.setOutlineThickness(1);

        auto& header = table.getHeader();
        header.addColumn("Component / Slot", 1, 190, 120, 300);
        header.addColumn("Loaded Plugin / Asset", 2, 230, 140, 400);
        header.addColumn("DSP CPU %", 3, 110, 80, 160);
        header.addColumn("Status", 4, 90, 60, 140);
        addAndMakeVisible(table);

        updateData();
        startTimer(1000);
        setSize(740, 520);
    }

    ~ResourceInspectorModal() override {
        stopTimer();
    }

    void timerCallback() override {
        updateData();
    }

    struct RowData {
        juce::String componentName;
        juce::String pluginName;
        float cpuUsage = 0.0f;
        bool hasCpu = false;
        juce::String status;
        bool isCore = false;
    };

    void updateData() {
        memStats = FanfareLog::getMemoryStats();

        rows.clear();
        float totalSlotDsp = 0.0f;

        std::vector<RowData> slotRows;
        for (int i = 0; i < engine.getNumSlots(); ++i) {
            if (auto* slot = engine.getSlot(i)) {
                RowData r;
                r.componentName = "Slot " + juce::String(i + 1) + " (" + slot->getName() + ")";
                juce::String pName = slot->getPluginName(0);
                if (pName.isEmpty()) pName = "(Empty / Passthrough)";
                r.pluginName = pName;
                r.status = slot->isBypassed() ? "Muted" : "Active";

                if (!slot->isBypassed()) {
                    r.cpuUsage = slot->getCpuUsage();
                    r.hasCpu = true;
                    totalSlotDsp += r.cpuUsage;
                }
                slotRows.push_back(r);
            }
        }

        std::vector<RowData> auxRows;
        for (int i = 0; i < engine.getNumAuxReturns(); ++i) {
            if (auto* slot = engine.getAuxReturn(i)) {
                RowData r;
                r.componentName = "Aux Return " + juce::String(i + 1);
                juce::String pName = slot->getPluginName(0);
                if (pName.isEmpty()) pName = "(Empty / Passthrough)";
                r.pluginName = pName;
                r.status = slot->isBypassed() ? "Muted" : "Active";

                if (!slot->isBypassed()) {
                    r.cpuUsage = slot->getCpuUsage();
                    r.hasCpu = true;
                    totalSlotDsp += r.cpuUsage;
                }
                auxRows.push_back(r);
            }
        }

        totalEngineDsp = totalSlotDsp;

        RowData coreRow;
        coreRow.componentName = "Fanfare Process Core";
        coreRow.pluginName = "JUCE Host Engine";
        coreRow.cpuUsage = totalEngineDsp;
        coreRow.hasCpu = true;
        coreRow.status = "Active";
        coreRow.isCore = true;
        rows.push_back(coreRow);

        for (const auto& sr : slotRows)
            rows.push_back(sr);
        for (const auto& ar : auxRows)
            rows.push_back(ar);

        table.setColour(juce::ListBox::backgroundColourId, ThemeManager::get(Theme::Role::sunken));
        table.setColour(juce::ListBox::outlineColourId, ThemeManager::get(Theme::Role::border));
        infoLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::textDim));
        table.updateContent();
        repaint();
    }

    int getNumRows() override {
        return (int)rows.size();
    }

    void paintRowBackground(juce::Graphics& g, int rowNumber, int, int, bool rowIsSelected) override {
        const auto& t = ThemeManager::getInstance().active();
        if (rowIsSelected) {
            g.fillAll(t.accent.withAlpha(t.dark ? 0.22f : 0.16f));
        } else if (rowNumber == 0) {
            g.fillAll(t.panelAlt);
        } else if (rowNumber % 2 == 1) {
            g.fillAll(t.text.withAlpha(0.02f));
        }
    }

    void paintCell(juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool) override {
        if (rowNumber < 0 || rowNumber >= (int)rows.size()) return;

        const auto& t = ThemeManager::getInstance().active();
        const auto& r = rows[rowNumber];
        g.setFont(columnId == 3 ? BoutiqueLookAndFeel::monoFont(11.5f, r.isCore)
                                : BoutiqueLookAndFeel::uiFont(12.5f, r.isCore));

        if (columnId == 1) {
            g.setColour(r.isCore ? t.accent : t.text);
            g.drawText(r.componentName, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
        } else if (columnId == 2) {
            g.setColour(r.isCore ? t.text : t.textDim);
            g.drawText(r.pluginName, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
        } else if (columnId == 3) {
            if (r.hasCpu) {
                float pct = r.cpuUsage * 100.0f;
                juce::Colour cpuColour = (pct >= 40.0f) ? t.danger : ((pct >= 15.0f) ? t.warn : t.ok);
                g.setColour(cpuColour);
                juce::String cpuText = (pct < 0.05f) ? "< 0.1%" : (juce::String(pct, 1) + "%");
                g.drawText(cpuText, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
            } else {
                g.setColour(t.textFaint);
                g.drawText("--", 8, 0, width - 16, height, juce::Justification::centredLeft, true);
            }
        } else if (columnId == 4) {
            g.setColour(r.status == "Active" ? t.ok : t.textDim);
            g.drawText(r.status, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
        }
    }

    static juce::String formatBytes(size_t bytes) {
        if (bytes >= 1024 * 1024 * 1024)
            return juce::String((double)bytes / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
        if (bytes >= 1024 * 1024)
            return juce::String((double)bytes / (1024.0 * 1024.0), 1) + " MB";
        if (bytes >= 1024)
            return juce::String(bytes / 1024) + " KB";
        return juce::String(bytes) + " B";
    }

    void paint(juce::Graphics& g) override {
        const auto& t = ThemeManager::getInstance().active();
        Fanfare::ModalFrame::paintOverlayScrimAndPanel(
            g, getLocalBounds(), getLocalBounds(), "RESOURCE & DSP INSPECTOR", 38);

        auto bounds = getLocalBounds().reduced(15);
        bounds.removeFromTop(36);

        auto cardRow = bounds.removeFromTop(68);
        const int numCards = 4;
        const int gap = 8;
        const int cardW = (cardRow.getWidth() - (gap * (numCards - 1))) / numCards;

        drawStatCard(g, cardRow.removeFromLeft(cardW), "PROCESS RAM (WS)",
                     formatBytes(memStats.workingSetBytes), "Physical RAM in use", t.accent);
        cardRow.removeFromLeft(gap);

        drawStatCard(g, cardRow.removeFromLeft(cardW), "COMMIT CHARGE",
                     formatBytes(memStats.privateCommitBytes), "Virtual Memory Allocated", t.text);
        cardRow.removeFromLeft(gap);

        float dspPct = totalEngineDsp * 100.0f;
        juce::String dspStr = (dspPct < 0.1f) ? "< 0.1%" : (juce::String(dspPct, 1) + "%");
        juce::Colour dspCol = (dspPct >= 40.0f) ? t.danger : ((dspPct >= 15.0f) ? t.warn : t.ok);
        drawStatCard(g, cardRow.removeFromLeft(cardW), "TOTAL DSP LOAD",
                     dspStr, "Real-Time Audio Thread", dspCol);
        cardRow.removeFromLeft(gap);

        drawStatCard(g, cardRow, "FREE SYSTEM RAM",
                     formatBytes(memStats.freeSystemRamBytes),
                     "Load: " + juce::String(memStats.systemRamLoadPercent) + "% of " + formatBytes(memStats.totalSystemRamBytes),
                     t.ok);
    }

    void drawStatCard(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title,
                      const juce::String& val, const juce::String& sub, juce::Colour accent) {
        const auto& t = ThemeManager::getInstance().active();
        Fanfare::ModalFrame::paintRecessedWell(g, area.toFloat(), 3.0f);

        g.setColour(t.engrave);
        g.setFont(BoutiqueLookAndFeel::condensedFont(10.5f, 0.08f));
        g.drawText(title, area.getX() + 8, area.getY() + 6, area.getWidth() - 16, 14, juce::Justification::left);

        g.setColour(accent);
        g.setFont(BoutiqueLookAndFeel::monoFont(16.0f, true));
        g.drawText(val, area.getX() + 8, area.getY() + 22, area.getWidth() - 16, 22, juce::Justification::left);

        g.setColour(t.textDim);
        g.setFont(BoutiqueLookAndFeel::uiFont(10.5f, false));
        g.drawText(sub, area.getX() + 8, area.getY() + 46, area.getWidth() - 16, 14, juce::Justification::left);
    }

    void resized() override {
        auto full = getLocalBounds();
        auto top = full.removeFromTop(38).reduced(10, 6);
        closeBtn.setBounds(top.removeFromRight(52));
        top.removeFromRight(4);
        helpBtn.setBounds(top.removeFromRight(68));

        int topY = 128;
        refreshBtn.setBounds(15, topY, 120, 26);
        infoLabel.setBounds(144, topY, getWidth() - 159, 26);
        table.setBounds(15, topY + 34, getWidth() - 30, getHeight() - topY - 46);
    }

private:
    FanfareEngine& engine;
    std::function<void()> closeCallback;
    juce::Label titleLabel;
    juce::Label infoLabel;
    juce::TextButton closeBtn;
    juce::TextButton helpBtn{"HAAALP!"};
    juce::TextButton refreshBtn;
    juce::TableListBox table;

    FanfareLog::ProcessMemoryStats memStats;
    float totalEngineDsp = 0.0f;
    std::vector<RowData> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ResourceInspectorModal)
};
