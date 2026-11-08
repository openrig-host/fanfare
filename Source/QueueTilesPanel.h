#pragma once

#include <JuceHeader.h>
#include "QueueButtonComponent.h"
#include "ThemeManager.h"

// QueueTilesPanel - hosts the 10 one-touch queue tiles (core + next 1..9) as
// list rows inside the Queue sidebar context, with the queue-map save/load
// actions above them. The tiles and buttons are owned by MainComponent (all
// their click / drop / MIDI wiring stays there); this panel only parents and
// lays them out. Replaces the old full-width queue row in the header.
namespace Fanfare {

class QueueTilesPanel : public juce::Component {
public:
    QueueTilesPanel() {
        tilesViewport.setViewedComponent(&tilesContainer, false);
        tilesViewport.setScrollBarsShown(true, false);
        tilesViewport.setScrollBarThickness(6);
        addAndMakeVisible(tilesViewport);
    }

    void setContent(juce::Array<QueueButtonComponent*> newTiles,
                    juce::Array<juce::Button*> newActions) {
        tiles = std::move(newTiles);
        actions = std::move(newActions);
        for (auto* tile : tiles) {
            tile->setRowStyle(true);
            tilesContainer.addAndMakeVisible(tile);
        }
        for (auto* b : actions)
            addAndMakeVisible(b);
        resized();
    }

    void paint(juce::Graphics& g) override {
        g.setColour(ThemeManager::get(Theme::Role::textDim));
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        const int rightTrim = actions.size() * 82 + 4;
        g.drawText("Live queue",
                   getLocalBounds().removeFromTop(24).withTrimmedRight(rightTrim),
                   juce::Justification::centredLeft, true);
    }

    void resized() override {
        auto b = getLocalBounds();
        auto top = b.removeFromTop(24);
        for (int i = actions.size(); --i >= 0;) {
            actions[i]->setBounds(top.removeFromRight(juce::jmin(78, top.getWidth())).reduced(1, 1));
            top.removeFromRight(4);
        }
        b.removeFromTop(4);
        tilesViewport.setBounds(b);
        const int n = tiles.size();
        const int rowH = (n == 0) ? 0 : juce::jlimit(22, 30, b.getHeight() / n);
        const int totalH = n * rowH;
        const bool needsScroll = totalH > b.getHeight();
        const int contentW = juce::jmax(1, b.getWidth() - (needsScroll ? 8 : 0));
        tilesContainer.setSize(contentW, totalH);
        int y = 0;
        for (auto* tile : tiles) {
            tile->setBounds(0, y, contentW, rowH);
            y += rowH;
        }
    }

private:
    juce::Viewport tilesViewport;
    juce::Component tilesContainer;
    juce::Array<QueueButtonComponent*> tiles;
    juce::Array<juce::Button*> actions;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(QueueTilesPanel)
};

} // namespace Fanfare
