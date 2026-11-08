#pragma once

#include <JuceHeader.h>
#include "BoutiqueLookAndFeel.h"
#include "FanfareSplashArtData.h"
#include "Version.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Fanfare {

inline const juce::Image& getBrandBadgeEmblem() {
    static const juce::Image img = juce::ImageFileFormat::loadFrom(
        SplashAssets::BADGE_EMBLEM_PNG, SplashAssets::BADGE_EMBLEM_PNG_LEN);
    return img;
}

inline const juce::Image& getSplashHeroArtwork() {
    static const juce::Image img = juce::ImageFileFormat::loadFrom(
        SplashAssets::SPLASH_ART_JPG, SplashAssets::SPLASH_ART_JPG_LEN);
    return img;
}

// Shared Fanfare brand icon badge (used in both FanfareSplashScreen and StartupWelcomeModal):
// dark Studio rounded square with Fanfare's signature forte 'f' (#FFA02F) + treble clef (#F4F5F7).
inline void drawBrandBadge(juce::Graphics& g, juce::Rectangle<float> box) {
    const float radius = box.getWidth() * 0.19f;
    g.setColour(juce::Colour(0xFF202226));
    g.fillRoundedRectangle(box, radius);
    g.setColour(juce::Colour(0xFF31343C));
    g.drawRoundedRectangle(box.reduced(0.5f), radius, 1.0f);

    const auto& emblem = getBrandBadgeEmblem();
    if (emblem.isValid()) {
        juce::Graphics::ScopedSaveState state(g);
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        const auto pad = box.getWidth() * 0.055f;
        g.drawImage(emblem, box.reduced(pad), juce::RectanglePlacement::centred);
    }
}

//==============================================================================
// Borderless Studio splash screen with synchronous pre-loop painting, sleek
// #FFA02F progress bar, Fanfare f-clef emblem badge, and bespoke stage-keyboard
// & fader spotlight artwork on the right pane.
class FanfareSplashScreen : public juce::Component, private juce::Timer {
public:
    std::function<void()> onFinished;

    FanfareSplashScreen() {
        activeInstance() = this;
        setOpaque(true);
        setSize(kWidth, kHeight);

        // Pre-decode splash art once before showing window so first paint is instant
        (void)getBrandBadgeEmblem();
        (void)getSplashHeroArtwork();

        const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        const auto area = display != nullptr ? display->userArea
                                             : juce::Rectangle<int>(0, 0, 1920, 1080);
        setBounds(area.withSizeKeepingCentre(kWidth, kHeight));

        addToDesktop(juce::ComponentPeer::windowIsTemporary |
                     juce::ComponentPeer::windowHasDropShadow);
        setAlwaysOnTop(true);
        setVisible(true);
        toFront(false);
        forceImmediatePaint();
    }

    ~FanfareSplashScreen() override {
        stopTimer();
        if (activeInstance() == this)
            activeInstance() = nullptr;
    }

    static void reportProgress(float progress01, const juce::String& statusText) {
        if (auto* inst = activeInstance())
            inst->setStage(progress01, statusText);
    }

    // Updates progress immediately on screen even before the main message loop runs.
    void setStage(float progress01, const juce::String& statusText) {
        targetProgress = juce::jlimit(0.0f, 1.0f, progress01);
        if (!isTimerRunning())
            displayedProgress = targetProgress;
        stageLabel = statusText;
        repaint();
        forceImmediatePaint();
    }

    // Smoothly completes the bar, holds briefly so the splash never flashes
    // abruptly, and fades out over ~240ms.
    void finishAndDismiss() {
        targetProgress = 1.0f;
        stageLabel = "Ready";
        closingPhase = Phase::Completing;
        holdFrames = 16; // ~260ms at 60 Hz
        startTimerHz(60);
    }

    void paint(juce::Graphics& g) override {
        const auto bounds = getLocalBounds().toFloat();
        constexpr float leftW = 220.0f;

        // 1. Left identity column (#151618)
        const juce::Rectangle<float> leftCol(0.0f, 0.0f, leftW, bounds.getHeight());
        g.setColour(juce::Colour(0xFF151618));
        g.fillRect(leftCol);

        // Fanfare f+clef brand badge
        drawBrandBadge(g, {34.0f, 104.0f, 54.0f, 54.0f});

        // Product name
        g.setColour(juce::Colour(0xFFF3F4F6));
        g.setFont(BoutiqueLookAndFeel::uiFont(29.0f, true));
        g.drawText("Fanfare", 34, 174, 168, 34, juce::Justification::centredLeft, false);

        // Version string in JetBrains Mono
        g.setColour(juce::Colour(0xFF8A8E96));
        g.setFont(BoutiqueLookAndFeel::monoFont(12.5f, false));
        g.drawText("Version " OPENRIG_VERSION_STRING, 34, 211, 168, 18,
                   juce::Justification::centredLeft, false);

        // Sleek #FFA02F pill progress bar
        const juce::Rectangle<float> trackRect(34.0f, 246.0f, 152.0f, 5.0f);
        g.setColour(juce::Colour(0xFF23252A));
        g.fillRoundedRectangle(trackRect, 2.5f);

        const float p = juce::jlimit(0.06f, 1.0f, displayedProgress);
        const float fillW = trackRect.getWidth() * p;
        g.setColour(juce::Colour(0xFFFFA02F));
        g.fillRoundedRectangle(trackRect.withWidth(fillW), 2.5f);

        if (stageLabel.isNotEmpty()) {
            g.setColour(juce::Colour(0xFF676B74));
            g.setFont(BoutiqueLookAndFeel::uiFont(11.5f, false));
            g.drawText(stageLabel, 34, 260, 172, 16, juce::Justification::centredLeft, true);
        }

        // 2. Right pane: bespoke Fanfare stage-keyboard & fader spotlight artwork
        const juce::Rectangle<float> rightPane(leftW, 0.0f, bounds.getWidth() - leftW, bounds.getHeight());
        {
            juce::Graphics::ScopedSaveState clipState(g);
            g.reduceClipRegion(rightPane.toNearestInt());
            g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);

            const auto& art = getSplashHeroArtwork();
            if (art.isValid()) {
                g.drawImage(art, rightPane, juce::RectanglePlacement::fillDestination);
            } else {
                g.setColour(juce::Colour(0xFF181A1E));
                g.fillRect(rightPane);
            }

            // Soft left-edge blend into the #151618 identity column
            juce::ColourGradient seamBlend(juce::Colour(0xEA151618), leftW, 0.0f,
                                           juce::Colour(0x00151618), leftW + 44.0f, 0.0f, false);
            g.setGradientFill(seamBlend);
            g.fillRect(juce::Rectangle<float>(leftW, 0.0f, 44.0f, bounds.getHeight()));

            // Subtle stage spotlight warm-up bloom as startup progresses
            const float bloomAlpha = 0.04f + 0.09f * displayedProgress;
            const float glowX = leftW + rightPane.getWidth() * 0.67f;
            const float glowY = bounds.getHeight() * 0.34f;
            juce::ColourGradient stageBloom(juce::Colour(0xFFFFA02F).withAlpha(bloomAlpha), glowX, glowY,
                                            juce::Colour(0x00FFA02F), glowX - 180.0f, glowY + 140.0f, true);
            g.setGradientFill(stageBloom);
            g.fillRect(rightPane);
        }

        // Hairline divider + outer 1px window frame
        g.setColour(juce::Colour(0x8826282D));
        g.drawVerticalLine((int)leftW, 0.0f, bounds.getHeight());

        g.setColour(juce::Colour(0xFF2E3036));
        g.drawRect(getLocalBounds(), 1);
    }

private:
    static constexpr int kWidth  = 620;
    static constexpr int kHeight = 380;

    enum class Phase { Idle, Completing, Holding, Fading };
    Phase closingPhase = Phase::Idle;
    float displayedProgress = 0.10f;
    float targetProgress = 0.10f;
    float windowAlpha = 1.0f;
    int holdFrames = 0;
    juce::String stageLabel{"Starting..."};

    static FanfareSplashScreen*& activeInstance() {
        static FanfareSplashScreen* s = nullptr;
        return s;
    }

    void forceImmediatePaint() {
#ifdef _WIN32
        if (auto* peer = getPeer()) {
            if (HWND hwnd = static_cast<HWND>(peer->getNativeHandle())) {
                ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
        }
#endif
    }

    void timerCallback() override {
        if (closingPhase == Phase::Completing) {
            displayedProgress += (targetProgress - displayedProgress) * 0.28f;
            if (std::abs(targetProgress - displayedProgress) < 0.01f) {
                displayedProgress = targetProgress;
                closingPhase = Phase::Holding;
            }
            repaint();
        } else if (closingPhase == Phase::Holding) {
            if (--holdFrames <= 0)
                closingPhase = Phase::Fading;
        } else if (closingPhase == Phase::Fading) {
            windowAlpha -= 0.075f;
            if (windowAlpha <= 0.02f) {
                stopTimer();
                setVisible(false);
                auto cb = std::move(onFinished);
                if (cb)
                    cb();
                return;
            }
            setAlpha(windowAlpha);
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FanfareSplashScreen)
};

} // namespace Fanfare
