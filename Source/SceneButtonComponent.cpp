#include "SceneButtonComponent.h"
#include "BoutiqueLookAndFeel.h"
#include "RigLibrary.h"

namespace Fanfare {

SceneButtonComponent::SceneButtonComponent(int index, const juce::String& name)
    : juce::Button("SceneButton_" + juce::String(index)), sceneIndex(index), defaultName(name) {
    setTooltip("Scene " + juce::String(index + 1) + " (Right-click to assign MIDI trigger or link a Song .json file)");
}

void SceneButtonComponent::setAssignedFile(const juce::File& file) {
    assignedFile = file;
    repaint();
}

void SceneButtonComponent::setMidiTriggerInfo(int pc, int channel) {
    midiPC = pc;
    midiChannel = channel;
    repaint();
}

void SceneButtonComponent::setActive(bool active) {
    if (isActive != active) {
        isActive = active;
        repaint();
    }
}

bool SceneButtonComponent::isInterestedInDragSource(const SourceDetails& dragSourceDetails) {
    juce::String desc = dragSourceDetails.description.toString();
    if (desc.isEmpty()) return false;
    juce::File f(desc);
    return f.getFileExtension().equalsIgnoreCase(".json") || desc.containsIgnoreCase(".json");
}

void SceneButtonComponent::itemDragEnter(const SourceDetails&) {
    isDragHover = true;
    repaint();
}

void SceneButtonComponent::itemDragExit(const SourceDetails&) {
    isDragHover = false;
    repaint();
}

void SceneButtonComponent::itemDropped(const SourceDetails& dragSourceDetails) {
    isDragHover = false;
    juce::String path = dragSourceDetails.description.toString();
    juce::File file(path);
    if (!file.existsAsFile()) {
        file = RigLibrary::getSongsDirectory().getChildFile(path);
    }

    juce::Component::SafePointer<SceneButtonComponent> safeThis(this);
    if (file.existsAsFile() && onFileDropped) {
        onFileDropped(sceneIndex, file);
    }
    if (safeThis != nullptr)
        safeThis->repaint();
}

void SceneButtonComponent::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) {
        if (onRightClicked) {
            onRightClicked(sceneIndex, e);
        }
        return;
    }

    juce::Component::SafePointer<SceneButtonComponent> safeThis(this);
    if (onClicked) {
        onClicked(sceneIndex);
    }

    if (safeThis != nullptr)
        juce::Button::mouseDown(e);
}

void SceneButtonComponent::paintButton(juce::Graphics& g,
                                       bool shouldDrawButtonAsHighlighted,
                                       bool shouldDrawButtonAsDown) {
    const auto& t = ThemeManager::getInstance().active();
    auto bounds = getLocalBounds().toFloat().reduced(0.5f);
    const float radius = 3.0f;
    const bool hasSetupFile = assignedFile.existsAsFile();

    juce::Colour topCol = isActive ? t.raised.interpolatedWith(t.accent, 0.14f) : t.faceTop;
    juce::Colour botCol = isActive ? t.panelAlt.interpolatedWith(t.accent, 0.08f) : t.faceBot;
    juce::Colour borderCol = isActive ? t.accent : t.borderStrong;

    if (shouldDrawButtonAsDown) {
        topCol = t.pressTop;
        botCol = t.pressBot;
        bounds = bounds.translated(0.0f, 1.0f);
    } else if (shouldDrawButtonAsHighlighted || isDragHover) {
        topCol = topCol.interpolatedWith(t.text, 0.06f);
        botCol = botCol.interpolatedWith(t.text, 0.04f);
    }

    if (isDragHover) {
        borderCol = t.accent;
    }

    juce::ColourGradient grad(topCol, bounds.getX(), bounds.getY(),
                              botCol, bounds.getX(), bounds.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(bounds, radius);

    if (!shouldDrawButtonAsDown) {
        g.setColour(t.bevelHi);
        g.drawHorizontalLine((int)bounds.getY() + 1, bounds.getX() + 2.0f, bounds.getRight() - 2.0f);
    }

    // Active scene gets a crisp 2px bottom accent bar (rather than a neon flood)
    if (isActive) {
        g.setColour(t.accent);
        g.fillRect(bounds.getX() + 2.0f, bounds.getBottom() - 2.5f,
                   bounds.getWidth() - 4.0f, 2.0f);
    }

    g.setColour(borderCol);
    g.drawRoundedRectangle(bounds, radius, (isActive || isDragHover) ? 1.5f : 1.0f);

    // Scene index tag on left + title centered
    juce::String title = hasSetupFile ? assignedFile.getFileNameWithoutExtension() : defaultName;
    auto textRect = bounds.reduced(8.0f, 2.0f);

    g.setFont(BoutiqueLookAndFeel::condensedFont(10.0f, 0.06f));
    g.setColour(isActive ? t.accent : t.engrave);
    g.drawText(juce::String(sceneIndex + 1), textRect.removeFromLeft(14.0f),
               juce::Justification::centredLeft, false);

    g.setFont(BoutiqueLookAndFeel::condensedFont(isActive ? 13.5f : 12.5f, 0.03f));
    g.setColour(isActive ? t.text : (hasSetupFile ? t.iem : t.textDim));
    g.drawText(title, textRect.withTrimmedRight(10.0f), juce::Justification::centred, true);

    // Subtle hardware LED indicator dot for linked Song or MIDI PC trigger
    if (hasSetupFile) {
        g.setColour(t.iem);
        g.fillEllipse(bounds.getRight() - 8.5f, bounds.getY() + 5.0f, 4.5f, 4.5f);
    } else if (midiPC >= 0) {
        g.setColour(t.warn);
        g.fillEllipse(bounds.getRight() - 8.5f, bounds.getY() + 5.0f, 4.5f, 4.5f);
    }
}

} // namespace Fanfare
