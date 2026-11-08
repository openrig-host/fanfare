#include "QueueButtonComponent.h"
#include "BoutiqueLookAndFeel.h"
#include "RigLibrary.h"

namespace Fanfare {

QueueButtonComponent::QueueButtonComponent(int index)
    : juce::Button("QueueButton_" + juce::String(index)), slotIndex(index) {
    setTooltip(slotIndex == 0 ? "CORE SONG (Default stage song - click to load, right-click or drag to assign)"
                              : "Stage Queue Slot " + juce::String(slotIndex) + " (Click to load, drag a .json song or right-click to assign)");
}

void QueueButtonComponent::setAssignedFile(const juce::File& file) {
    assignedFile = file;
    repaint();
}

void QueueButtonComponent::setSlotStates(bool active, bool preloaded, bool preloading) {
    bool changed = (isActive != active || isPreloaded != preloaded || isPreloading != preloading);
    isActive = active;
    isPreloaded = preloaded;
    isPreloading = preloading;
    if (changed) repaint();
}

bool QueueButtonComponent::isInterestedInDragSource(const SourceDetails& dragSourceDetails) {
    juce::String desc = dragSourceDetails.description.toString();
    if (desc.isEmpty()) return false;
    juce::File f(desc);
    return f.getFileExtension().equalsIgnoreCase(".json") || desc.containsIgnoreCase(".json");
}

void QueueButtonComponent::itemDragEnter(const SourceDetails&) {
    isDragHover = true;
    repaint();
}

void QueueButtonComponent::itemDragExit(const SourceDetails&) {
    isDragHover = false;
    repaint();
}

void QueueButtonComponent::itemDropped(const SourceDetails& dragSourceDetails) {
    isDragHover = false;
    juce::String path = dragSourceDetails.description.toString();
    juce::File file(path);
    if (!file.existsAsFile()) {
        file = RigLibrary::getSongsDirectory().getChildFile(path);
    }

    if (file.existsAsFile() && onFileDropped) {
        onFileDropped(slotIndex, file);
    }
    repaint();
}

void QueueButtonComponent::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) {
        juce::PopupMenu menu;
        menu.addSectionHeader(slotIndex == 0 ? "CORE SONG SLOT" : "QUEUE SLOT " + juce::String(slotIndex));
        menu.addItem(1, "Assign Song File...");
        if (assignedFile.existsAsFile()) {
            menu.addItem(2, "Clear Slot");
        }
        menu.showMenuAsync(juce::PopupMenu::Options(), [this](int result) {
            if (result == 1 && onAssignRequested) {
                onAssignRequested(slotIndex);
            } else if (result == 2 && onClearRequested) {
                onClearRequested(slotIndex);
            }
        });
        return;
    }
    juce::Button::mouseDown(e);
}

void QueueButtonComponent::paintRow(juce::Graphics& g, bool isOver, bool isDown) {
    const auto& t = ThemeManager::getInstance().active();
    auto r = getLocalBounds().toFloat().reduced(0.0f, 1.0f);
    const bool isCore = (slotIndex == 0);
    const bool hasSong = assignedFile.existsAsFile();

    juce::Colour bg = isDown ? t.pressTop
                    : (isActive ? t.accent.withAlpha(0.14f)
                    : ((isOver || isDragHover) ? t.panelAlt : juce::Colours::transparentBlack));
    g.setColour(bg);
    g.fillRoundedRectangle(r, 4.0f);
    if (isDragHover) {
        g.setColour(t.accent);
        g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.0f);
    }
    if (isActive) {
        g.setColour(t.accent);
        g.fillRoundedRectangle(r.getX(), r.getY() + 4.0f, 2.0f, r.getHeight() - 8.0f, 1.0f);
    }

    auto inner = r.reduced(8.0f, 0.0f);
    g.setFont(BoutiqueLookAndFeel::monoFont(11.0f));
    g.setColour(isActive ? t.accent : t.textFaint);
    g.drawText(isCore ? "core" : juce::String(slotIndex).paddedLeft('0', 2),
               inner.removeFromLeft(34.0f), juce::Justification::centredLeft, false);

    juce::String state;
    juce::Colour stateCol = t.textFaint;
    if (isActive)          { state = "live";    stateCol = t.accent; }
    else if (isPreloaded)  { state = "ready";   stateCol = t.ok; }
    else if (isPreloading) { state = "loading"; stateCol = t.warn; }
    if (state.isNotEmpty()) {
        g.setColour(stateCol);
        g.drawText(state, inner.removeFromRight(52.0f), juce::Justification::centredRight, false);
    }

    g.setFont(BoutiqueLookAndFeel::uiFont(13.0f, isActive));
    g.setColour(hasSong ? t.text : t.textFaint);
    g.drawText(hasSong ? assignedFile.getFileNameWithoutExtension()
                       : (isCore ? "Core song (drop a song here)" : "Empty - drop a song"),
               inner, juce::Justification::centredLeft, true);
}

void QueueButtonComponent::paintButton(juce::Graphics& g,
                                       bool shouldDrawButtonAsHighlighted,
                                       bool shouldDrawButtonAsDown) {
    if (rowStyle) {
        paintRow(g, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
        return;
    }
    const auto& t = ThemeManager::getInstance().active();
    auto bounds = getLocalBounds().toFloat().reduced(0.5f);
    const float radius = 3.0f;

    const bool isCore = (slotIndex == 0);
    const bool hasSong = assignedFile.existsAsFile();

    // Hardware cap vs recessed empty slot
    juce::Colour topCol = hasSong || isCore ? t.faceTop : t.sunken;
    juce::Colour botCol = hasSong || isCore ? t.faceBot : t.sunken;
    juce::Colour borderCol = isActive ? t.accent : (hasSong ? t.borderStrong : t.hairline);

    if (isActive) {
        topCol = t.raised.interpolatedWith(t.accent, 0.14f);
        botCol = t.panelAlt.interpolatedWith(t.accent, 0.08f);
    } else if (isPreloaded) {
        topCol = t.faceTop.interpolatedWith(t.ok, 0.10f);
        botCol = t.faceBot.interpolatedWith(t.ok, 0.06f);
        borderCol = t.ok.withAlpha(0.75f);
    }

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

    // Top bevel highlight on populated caps
    if ((hasSong || isCore) && !shouldDrawButtonAsDown) {
        g.setColour(t.bevelHi);
        g.drawHorizontalLine((int)bounds.getY() + 1, bounds.getX() + 2.0f, bounds.getRight() - 2.0f);
    }

    // Bottom 2px status bar (Active = teal, Ready = ok sage, Preloading = warn amber, Assigned = dim hairline)
    if (isActive || isPreloaded || isPreloading || hasSong) {
        juce::Colour barCol = isActive ? t.accent
                            : (isPreloaded ? t.ok
                            : (isPreloading ? t.warn : t.borderStrong));
        g.setColour(barCol);
        g.fillRect(bounds.getX() + 2.0f, bounds.getBottom() - 2.5f,
                   bounds.getWidth() - 4.0f, 2.0f);
    }

    g.setColour(borderCol);
    g.drawRoundedRectangle(bounds, radius, (isActive || isDragHover) ? 1.5f : 1.0f);

    // Top engraved index row
    auto inner = bounds.reduced(5.0f, 2.0f);
    auto tagArea = inner.removeFromTop(12.0f);
    g.setFont(BoutiqueLookAndFeel::condensedFont(10.0f, 0.08f));
    g.setColour(isCore ? t.accent : t.engrave);
    g.drawText(isCore ? "CORE" : juce::String(slotIndex).paddedLeft('0', 2),
               tagArea, juce::Justification::centredLeft, false);

    if (isActive) {
        g.setColour(t.accent);
        g.drawText("LIVE", tagArea, juce::Justification::centredRight, false);
    } else if (isPreloaded) {
        g.setColour(t.ok);
        g.drawText("READY", tagArea, juce::Justification::centredRight, false);
    } else if (isPreloading) {
        g.setColour(t.warn);
        g.drawText("LOAD", tagArea, juce::Justification::centredRight, false);
    }

    // Main Song Title
    juce::String title = hasSong ? assignedFile.getFileNameWithoutExtension()
                                 : (isCore ? "CORE SONG" : "EMPTY");
    g.setFont(BoutiqueLookAndFeel::condensedFont(hasSong ? 13.5f : 11.5f, hasSong ? 0.02f : 0.06f));
    g.setColour(hasSong ? t.text : (isCore ? t.textDim : t.textFaint));
    g.drawText(title, inner.withTrimmedBottom(2.0f), juce::Justification::centred, true);
}

} // namespace Fanfare
