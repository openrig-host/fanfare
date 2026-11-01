#pragma once

#include <JuceHeader.h>
#include "ButtonRoles.h"
#include "ThemeManager.h"

class WaveformSpliceEditor : public juce::Component,
                             public juce::ChangeListener {
public:
    WaveformSpliceEditor(std::function<void(float, float)> onRatiosChanged)
        : cache(64),
          thumbnail(256, formatManager, cache),
          onChangedCallback(onRatiosChanged)
    {
        formatManager.registerBasicFormats();
        thumbnail.addChangeListener(this);
    }

    ~WaveformSpliceEditor() override {
        thumbnail.removeChangeListener(this);
    }

    void setFile(const juce::File& file, float currentStart, float currentEnd) {
        startRatio = currentStart;
        endRatio = currentEnd;

        if (file.existsAsFile()) {
            thumbnail.setSource(new juce::FileInputSource(file));
        } else {
            thumbnail.clear();
        }
        repaint();
    }

    void setRatios(float start, float end) {
        startRatio = start;
        endRatio = end;
        repaint();
    }

    void paint(juce::Graphics& g) override {
        auto bounds = getLocalBounds().toFloat();

        // Recessed hardware well
        Fanfare::ModalFrame::paintRecessedWell(g, bounds, 3.0f);

        // Draw Waveform
        if (thumbnail.getTotalLength() > 0.0) {
            g.setColour(ThemeManager::get(Theme::Role::accent).withAlpha(0.75f));
            thumbnail.drawChannels(g, getLocalBounds().reduced(3), 0.0, thumbnail.getTotalLength(), 1.0f);

            // Shading out-of-range areas
            float startX = startRatio * getWidth();
            float endX = endRatio * getWidth();

            g.setColour(ThemeManager::get(Theme::Role::canvas).withAlpha(0.65f));
            g.fillRect(juce::Rectangle<float>(1.0f, 1.0f, juce::jmax(0.0f, startX - 1.0f), (float)getHeight() - 2.0f));
            g.fillRect(juce::Rectangle<float>(endX, 1.0f, juce::jmax(0.0f, (float)getWidth() - endX - 1.0f), (float)getHeight() - 2.0f));

            // Start = Gold (FOH), End = Cyan (IEM)
            const auto startCol = ThemeManager::get(Theme::Role::foh);
            const auto endCol   = ThemeManager::get(Theme::Role::iem);

            g.setColour(startCol);
            g.drawVerticalLine((int)startX, 1.0f, (float)getHeight() - 1.0f);
            g.fillRect(startX - 3.5f, 1.0f, 7.0f, 7.0f);

            g.setColour(endCol);
            g.drawVerticalLine((int)endX, 1.0f, (float)getHeight() - 1.0f);
            g.fillRect(endX - 3.5f, 1.0f, 7.0f, 7.0f);
        } else {
            g.setColour(ThemeManager::get(Theme::Role::textDim));
            g.setFont(juce::FontOptions("Barlow Condensed", 13.5f, juce::Font::bold));
            g.drawText("SELECT A LOADED PAD TO VIEW OR SLICE WAVEFORM", getLocalBounds(), juce::Justification::centred);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (thumbnail.getNumChannels() == 0) return;

        float x = (float)e.x;
        float startX = startRatio * getWidth();
        float endX = endRatio * getWidth();

        if (std::abs(x - startX) < std::abs(x - endX)) {
            draggingStart = true;
        } else {
            draggingStart = false;
        }

        updateHandlePosition(x);
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (thumbnail.getNumChannels() == 0) return;
        updateHandlePosition((float)e.x);
    }

    void changeListenerCallback(juce::ChangeBroadcaster* /*source*/) override {
        repaint();
    }

private:
    void updateHandlePosition(float mouseX) {
        float ratio = juce::jlimit(0.0f, 1.0f, mouseX / getWidth());

        if (draggingStart) {
            startRatio = juce::jmin(ratio, endRatio - 0.01f);
        } else {
            endRatio = juce::jmax(ratio, startRatio + 0.01f);
        }

        if (onChangedCallback) {
            onChangedCallback(startRatio, endRatio);
        }
        repaint();
    }

    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache cache;
    juce::AudioThumbnail thumbnail;

    std::function<void(float, float)> onChangedCallback;

    float startRatio = 0.0f;
    float endRatio = 1.0f;
    bool draggingStart = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformSpliceEditor)
};
