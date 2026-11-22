#pragma once

#include <JuceHeader.h>
#include "ThemeManager.h"
#include "BoutiqueLookAndFeel.h"
#include <vector>

namespace Fanfare {

struct SnapshotTileData {
    juce::String name;
    juce::uint32 colorArgb = 0; // 0 = default theme
    int midiPC = -1;
    int midiChannel = 0;
    bool active = false;
};

struct SnapshotColorChoice {
    const char* label;
    juce::uint32 argb;
};

inline const SnapshotColorChoice* getSnapshotPalette(int& count) noexcept {
    static const SnapshotColorChoice kPalette[] = {
        { "Default", 0x00000000 },
        { "Amber",   0xFFF59E0B },
        { "Coral",   0xFFEF4444 },
        { "Emerald", 0xFF10B981 },
        { "Cyan",    0xFF06B6D4 },
        { "Blue",    0xFF3B82F6 },
        { "Violet",  0xFF8B5CF6 },
        { "Gold",    0xFFEAB308 },
        { "Rose",    0xFFEC4899 },
    };
    count = (int)(sizeof(kPalette) / sizeof(kPalette[0]));
    return kPalette;
}

class SnapshotRailComponent : public juce::Component,
                              public juce::SettableTooltipClient {
public:
    std::function<void(int)> onRecall;
    std::function<void(int, juce::Component*)> onRightClick;
    std::function<void()> onAdd;
    std::function<void()> onUpdateActive;
    std::function<void()> onCollapsedChanged;

    SnapshotRailComponent() {
        collapseBtn.setTooltip("Collapse / Expand Snapshots rail");
        collapseBtn.onClick = [this] { setCollapsed(!collapsed); };
        addAndMakeVisible(collapseBtn);

        viewport.setScrollBarsShown(false, false, true, false);
        viewport.setViewedComponent(&listContainer, false);
        addAndMakeVisible(viewport);

        addBtn.setButtonText("+");
        addBtn.setTooltip("Capture current strip mutes, levels, ranges & DSP as a new Snapshot");
        addBtn.onClick = [this] {
            if (onAdd)
                onAdd();
        };
        addAndMakeVisible(addBtn);

        updateBtn.setButtonText("UPD");
        updateBtn.setTooltip("Overwrite active Snapshot with current strip mutes, levels, ranges & DSP");
        updateBtn.onClick = [this] {
            if (onUpdateActive)
                onUpdateActive();
        };
        addAndMakeVisible(updateBtn);
    }

    int getPreferredWidth() const noexcept {
        return collapsed ? 22 : 84;
    }

    bool isCollapsed() const noexcept { return collapsed; }

    void setCollapsed(bool shouldCollapse) {
        if (collapsed == shouldCollapse)
            return;
        collapsed = shouldCollapse;
        collapseBtn.setButtonText(collapsed ? "<" : ">");
        viewport.setVisible(!collapsed);
        addBtn.setVisible(!collapsed);
        updateBtn.setVisible(!collapsed);
        resized();
        repaint();
        if (onCollapsedChanged)
            onCollapsedChanged();
    }

    void setSnapshots(const std::vector<SnapshotTileData>& items) {
        tiles.clear();
        listContainer.removeAllChildren();
        for (int i = 0; i < (int)items.size(); ++i) {
            auto* tile = new TileButton(i, items[(size_t)i]);
            tile->onLeftClick = [this](int idx) {
                if (onRecall)
                    onRecall(idx);
            };
            tile->onPopupClick = [this](int idx, juce::Component* src) {
                if (onRightClick)
                    onRightClick(idx, src);
            };
            listContainer.addAndMakeVisible(tile);
            tiles.add(tile);
        }
        layoutList();
        repaint();
    }

    void setActiveIndex(int activeIdx) {
        for (int i = 0; i < tiles.size(); ++i)
            tiles[i]->setActive(i == activeIdx);
    }

    void refreshStyles() {
        const auto& t = ThemeManager::getInstance().active();
        collapseBtn.setButtonText(collapsed ? "<" : ">");
        setButtonRole(collapseBtn, ButtonRole::Subtle);
        setButtonRole(addBtn, ButtonRole::Subtle);
        setButtonRole(updateBtn, ButtonRole::Subtle);
        juce::ignoreUnused(t);
        repaint();
        for (auto* tile : tiles)
            tile->repaint();
    }

    void paint(juce::Graphics& g) override {
        const auto& t = ThemeManager::getInstance().active();
        g.fillAll(t.background);
        g.setColour(t.border);
        g.drawVerticalLine(0, 0.0f, (float)getHeight());

        if (collapsed) {
            g.setColour(t.engrave);
            g.setFont(BoutiqueLookAndFeel::condensedFont(10.0f, 0.14f));
            const int cy = getHeight() / 2 + 12;
            g.saveState();
            g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi,
                                                            (float)getWidth() * 0.5f,
                                                            (float)cy));
            g.drawText("SNAPS", getWidth() / 2 - 34, cy - 8, 68, 16,
                       juce::Justification::centred, false);
            g.restoreState();
        } else {
            g.setColour(t.engrave);
            g.setFont(BoutiqueLookAndFeel::condensedFont(10.0f, 0.10f));
            g.drawText("SNAPS", 6, 4, getWidth() - 28, 18,
                       juce::Justification::centredLeft, false);
            g.setColour(t.border);
            g.drawHorizontalLine(24, 4.0f, (float)getWidth() - 4.0f);
            g.drawHorizontalLine(getHeight() - 28, 4.0f, (float)getWidth() - 4.0f);
        }
    }

    void mouseUp(const juce::MouseEvent& e) override {
        if (collapsed && e.mouseWasClicked() && getLocalBounds().contains(e.getPosition()))
            setCollapsed(false);
    }

    void resized() override {
        if (collapsed) {
            collapseBtn.setBounds(2, 4, getWidth() - 4, 20);
            viewport.setBounds({});
            addBtn.setBounds({});
            updateBtn.setBounds({});
            return;
        }

        auto r = getLocalBounds();
        auto hdr = r.removeFromTop(24);
        collapseBtn.setBounds(hdr.removeFromRight(22).reduced(2, 3));

        auto ftr = r.removeFromBottom(28).reduced(4, 3);
        const int half = ftr.getWidth() / 2;
        addBtn.setBounds(ftr.removeFromLeft(half).reduced(1, 0));
        updateBtn.setBounds(ftr.reduced(1, 0));

        viewport.setBounds(r.reduced(2, 3));
        layoutList();
    }

private:
    class TileButton : public juce::Component, public juce::SettableTooltipClient {
    public:
        std::function<void(int)> onLeftClick;
        std::function<void(int, juce::Component*)> onPopupClick;

        TileButton(int idx, const SnapshotTileData& d) : index(idx), data(d) {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            juce::String tip = juce::String(idx + 1) + ". " + d.name;
            if (d.midiPC >= 0) {
                tip << "  [PC " << d.midiPC;
                if (d.midiChannel > 0)
                    tip << " Ch " << d.midiChannel;
                tip << "]";
            }
            tip << "\nClick: recall instant snapshot (no VST reload)\nRight-click: update, rename, color, MIDI learn";
            setTooltip(tip);
        }

        void setActive(bool a) {
            if (data.active != a) {
                data.active = a;
                repaint();
            }
        }

        void paint(juce::Graphics& g) override {
            const auto& t = ThemeManager::getInstance().active();
            auto b = getLocalBounds().toFloat().reduced(0.5f);
            const float radius = 3.0f;
            const bool over = isMouseOver();
            const bool hasCustomCol = (data.colorArgb != 0);
            const juce::Colour stripeCol = hasCustomCol ? juce::Colour(data.colorArgb)
                                                        : (data.active ? t.accent : t.borderStrong);

            juce::Colour face = data.active
                ? (hasCustomCol ? t.raised.interpolatedWith(stripeCol, 0.22f)
                                : t.raised.interpolatedWith(t.accent, 0.16f))
                : (over ? t.panelAlt : (hasCustomCol ? t.panel.interpolatedWith(stripeCol, 0.08f) : t.panel));

            g.setColour(face);
            g.fillRoundedRectangle(b, radius);

            // 4px left color accent stripe
            g.setColour(stripeCol);
            g.fillRoundedRectangle(b.getX() + 1.0f, b.getY() + 1.5f, 3.5f, b.getHeight() - 3.0f, 1.5f);

            g.setColour(data.active ? (hasCustomCol ? stripeCol : t.accent)
                                    : (over ? t.borderStrong : t.border));
            g.drawRoundedRectangle(b, radius, data.active ? 1.4f : 0.9f);

            // Content row: small index + compact ~8-char label
            auto textRect = b.reduced(2.0f, 1.0f);
            textRect.removeFromLeft(5.0f);

            g.setFont(BoutiqueLookAndFeel::monoFont(9.0f, data.active));
            g.setColour(data.active ? t.accent : t.textDim);
            g.drawText(juce::String(index + 1), textRect.removeFromLeft(11.0f),
                       juce::Justification::centredLeft, false);

            if (data.midiPC >= 0) {
                g.setColour(t.warn);
                g.fillEllipse(b.getRight() - 6.0f, b.getY() + 3.0f, 3.5f, 3.5f);
            }

            juce::String shortName = data.name.trim();
            if (shortName.length() > 9)
                shortName = shortName.substring(0, 8) + "..";
            g.setFont(BoutiqueLookAndFeel::condensedFont(11.0f, 0.02f));
            g.setColour(data.active ? t.text : t.textDim.interpolatedWith(t.text, 0.45f));
            g.drawText(shortName, textRect, juce::Justification::centredLeft, false);
        }

        void mouseDown(const juce::MouseEvent& e) override {
            if (e.mods.isPopupMenu()) {
                if (onPopupClick)
                    onPopupClick(index, this);
                return;
            }
            if (onLeftClick)
                onLeftClick(index);
        }

        void mouseEnter(const juce::MouseEvent&) override { repaint(); }
        void mouseExit(const juce::MouseEvent&) override { repaint(); }

    private:
        int index = 0;
        SnapshotTileData data;
    };

    void layoutList() {
        const int w = juce::jmax(10, viewport.getWidth());
        const int tileH = 26;
        const int gap = 3;
        int y = 0;
        for (auto* t : tiles) {
            t->setBounds(1, y, w - 2, tileH);
            y += tileH + gap;
        }
        listContainer.setSize(w, juce::jmax(viewport.getHeight(), y));
    }

    bool collapsed = false;
    juce::TextButton collapseBtn{">"};
    juce::TextButton addBtn{"+"};
    juce::TextButton updateBtn{"UPD"};
    juce::Viewport viewport;
    juce::Component listContainer;
    juce::OwnedArray<TileButton> tiles;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SnapshotRailComponent)
};

} // namespace Fanfare
