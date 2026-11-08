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
    QueueTilesPanel() = default;

    void setContent(juce::Array<QueueButtonComponent*> newTiles,
                    juce::Array<juce::Button*> newActions) {
        tiles = std::move(newTiles);
        actions = std::move(newActions);
        for (auto* tile : tiles) {
            tile->setRowStyle(true);
            addAndMakeVisible(tile);
        }
        for (auto* b : actions)
            addAndMakeVisible(b);
        resized();
    }

    void paint(juce::Graphics& g) override {
        g.setColour(ThemeManager::get(Theme::Role::textDim));
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText("Live queue", getLocalBounds().removeFromTop(24).withTrimmedRight(160),
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
        const int rowH = tiles.isEmpty() ? 0 : juce::jlimit(22, 30, b.getHeight() / tiles.size());
        for (auto* tile : tiles)
            tile->setBounds(b.removeFromTop(rowH));
    }

private:
    juce::Array<QueueButtonComponent*> tiles;
    juce::Array<juce::Button*> actions;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(QueueTilesPanel)
};

} // namespace Fanfare
