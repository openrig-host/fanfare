#pragma once

#include <JuceHeader.h>
#include "BoutiqueLookAndFeel.h"
#include "Version.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Fanfare {

// Shared brand icon badge (used in both FanfareSplashScreen and StartupWelcomeModal):
// dark rounded square with recessed dial ring, #FFA02F orange sweep arc, and crisp white needle.
inline void drawBrandBadge(juce::Graphics& g, juce::Rectangle<float> box) {
    const float radius = box.getWidth() * 0.21f;
    g.setColour(juce::Colour(0xFF202226));
    g.fillRoundedRectangle(box, radius);
    g.setColour(juce::Colour(0xFF2F3138));
    g.drawRoundedRectangle(box.reduced(0.5f), radius, 1.0f);

    const float cx = box.getCentreX();
    const float cy = box.getCentreY() + box.getHeight() * 0.03f;
    const float r  = box.getWidth() * 0.29f;
    const float strokeW = juce::jmax(2.2f, box.getWidth() * 0.068f);

    const float startAng = juce::MathConstants<float>::pi * 1.22f; // ~7:30
    const float endAng   = juce::MathConstants<float>::pi * 2.78f; // ~4:30
    const float valAng   = juce::MathConstants<float>::pi * 2.16f; // ~1:00

    // Inactive dark track arc
    juce::Path track;
    track.addCentredArc(cx, cy, r, r, 0.0f, startAng, endAng, true);
    g.setColour(juce::Colour(0xFF32353D));
    g.strokePath(track, juce::PathStrokeType(strokeW, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    // Signature #FFA02F orange arc
    juce::Path arc;
    arc.addCentredArc(cx, cy, r, r, 0.0f, startAng, valAng, true);
    g.setColour(juce::Colour(0xFFFFA02F));
    g.strokePath(arc, juce::PathStrokeType(strokeW, juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded));

    // Crisp white pointer needle from center toward 1 o'clock
    const float needleAngle = valAng - juce::MathConstants<float>::halfPi;
    const float nx0 = cx + std::cos(needleAngle) * (r * 0.12f);
    const float ny0 = cy + std::sin(needleAngle) * (r * 0.12f);
    const float nx1 = cx + std::cos(needleAngle) * (r * 1.02f);
    const float ny1 = cy + std::sin(needleAngle) * (r * 1.02f);
    g.setColour(juce::Colour(0xFFF4F5F7));
    g.drawLine(nx0, ny0, nx1, ny1, strokeW * 0.78f);
}

//==============================================================================
// Borderless Studio splash screen with synchronous pre-loop painting, smooth
// progress pill, and orbital stage-routing artwork on the right pane.
class FanfareSplashScreen : public juce::Component, private juce::Timer {
public:
    std::function<void()> onFinished;

    FanfareSplashScreen() {
        activeInstance() = this;
        setOpaque(true);
        setSize(kWidth, kHeight);

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

    // Smoothly completes the bar + needle sweep, holds briefly so the splash
    // never flashes abruptly, and fades out over 240ms.
    void finishAndDismiss() {
        targetProgress = 1.0f;
        stageLabel = "Ready";
        closingPhase = Phase::Completing;
        holdFrames = 16; // ~260ms at 60 Hz
        startTimerHz(60);
    }

    void paint(juce::Graphics& g) override {
        const auto bounds = getLocalBounds().toFloat();
        constexpr float leftW = 214.0f;

        // 1. Left identity column (#151618)
        juce::Rectangle<float> leftCol(0.0f, 0.0f, leftW, bounds.getHeight());
        g.setColour(juce::Colour(0xFF151618));
        g.fillRect(leftCol);

        // Brand badge
        drawBrandBadge(g, {34.0f, 98.0f, 48.0f, 48.0f});

        // Product name
        g.setColour(juce::Colour(0xFFF0F1F4));
        g.setFont(BoutiqueLookAndFeel::uiFont(30.0f, true));
        g.drawText("Fanfare", 34, 162, 160, 34, juce::Justification::centredLeft, false);

        // Version string
        g.setColour(juce::Colour(0xFF888A92));
        g.setFont(BoutiqueLookAndFeel::uiFont(14.5f, false));
        g.drawText("Version " OPENRIG_VERSION_STRING, 34, 200, 160, 20,
                   juce::Justification::centredLeft, false);

        // Sleek Nota-style pill progress bar
        const juce::Rectangle<float> trackRect(34.0f, 238.0f, 144.0f, 5.5f);
        g.setColour(juce::Colour(0xFF0C0D0F));
        g.fillRoundedRectangle(trackRect, 2.75f);

        const float p = juce::jlimit(0.06f, 1.0f, displayedProgress);
        const float fillW = trackRect.getWidth() * p;
        g.setColour(juce::Colour(0xFFFFA02F));
        g.fillRoundedRectangle(trackRect.withWidth(fillW), 2.75f);

        if (stageLabel.isNotEmpty()) {
            g.setColour(juce::Colour(0xFF5D6068));
            g.setFont(BoutiqueLookAndFeel::uiFont(11.5f, false));
            g.drawText(stageLabel, 34, 252, 164, 16, juce::Justification::centredLeft, true);
        }

        // 2. Right orbital console artwork (#1B1D20 + subtle radial teal-slate glow)
        const juce::Rectangle<float> rightPane(leftW, 0.0f, bounds.getWidth() - leftW, bounds.getHeight());
        {
            juce::Graphics::ScopedSaveState clipState(g);
            g.reduceClipRegion(rightPane.toNearestInt());

            g.setColour(juce::Colour(0xFF1A1C1F));
            g.fillRect(rightPane);

            const float hubX = leftW + rightPane.getWidth() * 0.72f;
            const float hubY = bounds.getHeight() * 0.515f;

            juce::ColourGradient glow(juce::Colour(0xFF27393D), hubX, hubY,
                                      juce::Colour(0xFF17191C), leftW + 10.0f, 24.0f, true);
            glow.addColour(0.52, juce::Colour(0xFF1D2427));
            g.setGradientFill(glow);
            g.fillRect(rightPane);

            // Concentric orbital rings + strip/scene node markers
            const float radii[3]   = {48.0f, 110.0f, 178.0f};
            const int   nodeCnt[3] = {6, 8, 12};
            const float phaseOff[3]= {0.35f, 0.18f, 0.08f};

            for (int ri = 0; ri < 3; ++ri) {
                const float rr = radii[ri];
                g.setColour(juce::Colour(0xFF393C42));
                g.drawEllipse(hubX - rr, hubY - rr, rr * 2.0f, rr * 2.0f, 2.4f);

                g.setColour(juce::Colour(0xFF2E3137));
                for (int n = 0; n < nodeCnt[ri]; ++n) {
                    const float a = phaseOff[ri] +
                                    juce::MathConstants<float>::twoPi * (float)n / (float)nodeCnt[ri];
                    const float nx = hubX + std::cos(a) * rr;
                    const float ny = hubY + std::sin(a) * rr;
                    g.fillEllipse(nx - 3.4f, ny - 3.4f, 6.8f, 6.8f);
                }
            }

            // Highlighted bus/routing nodes (cyan-blue stereo buses + silver reference node)
            auto drawAccentNode = [&](float radius, float angleRad, juce::Colour col) {
                const float nx = hubX + std::cos(angleRad) * radius;
                const float ny = hubY + std::sin(angleRad) * radius;
                g.setColour(juce::Colour(0xFF1A1C1F));
                g.fillEllipse(nx - 10.0f, ny - 10.0f, 20.0f, 20.0f);
                g.setColour(col);
                g.fillEllipse(nx - 8.2f, ny - 8.2f, 16.4f, 16.4f);
            };

            drawAccentNode(radii[0], -1.12f, juce::Colour(0xFF9EA3AC)); // inner silver node
            drawAccentNode(radii[1],  2.86f, juce::Colour(0xFF55B4D4)); // middle upper-left cyan
            drawAccentNode(radii[1],  1.95f, juce::Colour(0xFF55B4D4)); // middle lower-left cyan
            drawAccentNode(radii[2], -0.62f, juce::Colour(0xFF55B4D4)); // outer upper-right cyan

            // Signature #FFA02F orange vector ray sweeping subtly with progress
            const float sweepAngle = 0.22f + displayedProgress * 0.16f; // ~13 deg -> ~22 deg
            const float rayLen = 220.0f;
            const float rx = hubX + std::cos(sweepAngle) * rayLen;
            const float ry = hubY + std::sin(sweepAngle) * rayLen;
            g.setColour(juce::Colour(0xFFFFA02F));
            g.drawLine(hubX, hubY, rx, ry, 3.2f);
        }

        // Divider between left identity column and right artwork + subtle 1px card border
        g.setColour(juce::Colour(0xFF24262B));
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
