#pragma once

#include <JuceHeader.h>
#include "BoutiqueLookAndFeel.h"
#include "ThemeManager.h"
#include <cmath>

/**
 * AnalogVuMeter
 *
 * A classic vintage analog needle VU meter in a Front Panel chassis bezel.
 * Features a warm cream faceplate, curved dB arc (-20 to +3 dB), red overload zone,
 * dynamic needle ballistics, and a red PEAK clip LED.
 */
class AnalogVuMeter : public juce::Component, private juce::Timer {
public:
    AnalogVuMeter(const juce::String& labelText = "VU")
        : label(labelText)
    {
        startTimerHz(60);
    }

    ~AnalogVuMeter() override {
        stopTimer();
    }

    void setLevel(float linearLevel) {
        targetLevelLinear.store(linearLevel);
    }

    void paint(juce::Graphics& g) override {
        const auto& t = ThemeManager::getInstance().active();
        auto bounds = getLocalBounds().toFloat().reduced(1.0f);

        // 1. Recessed Front Panel Metal Bezel
        g.setColour(t.sunken);
        g.fillRoundedRectangle(bounds, 3.0f);

        g.setColour(t.borderStrong);
        g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

        auto face = bounds.reduced(3.5f);

        // 2. Warm Cream Hardware VU Faceplate (routed via Theme::Role::vuFace)
        juce::Colour vuBase = t.vuFace;
        juce::ColourGradient faceGrad(vuBase.brighter(0.06f), face.getX(), face.getY(),
                                      vuBase.darker(0.08f), face.getX(), face.getBottom(), false);
        g.setGradientFill(faceGrad);
        g.fillRoundedRectangle(face, 2.0f);

        g.setColour(juce::Colours::black.withAlpha(0.22f));
        g.drawRoundedRectangle(face, 2.0f, 1.0f);

        juce::Graphics::ScopedSaveState faceClip(g);
        g.reduceClipRegion(face.toNearestInt());

        float pivotX = face.getCentreX();
        float pivotY = face.getBottom() + (face.getHeight() * 0.25f);
        float radius = face.getHeight() * 0.95f;

        // 3. Scale Arc & Ticks (-20dB to +3dB)
        float startAngle = juce::degreesToRadians(-38.0f);
        float endAngle   = juce::degreesToRadians(38.0f);
        float zeroAngle  = dbToAngle(0.0f);

        juce::Path redArc;
        redArc.addCentredArc(pivotX, pivotY, radius - 4.0f, radius - 4.0f, 0.0f, zeroAngle, endAngle, true);
        g.setColour(juce::Colour(0xffcc3322).withAlpha(0.35f));
        g.strokePath(redArc, juce::PathStrokeType(5.5f));

        juce::Path scaleArc;
        scaleArc.addCentredArc(pivotX, pivotY, radius, radius, 0.0f, startAngle, endAngle, true);
        g.setColour(juce::Colour(0xff2a2825));
        g.strokePath(scaleArc, juce::PathStrokeType(1.1f));

        struct DbMark { float db; const char* text; bool isRed; };
        const DbMark marks[] = {
            {-20.0f, "20", false},
            {-10.0f, "10", false},
            {-7.0f,  "7",  false},
            {-5.0f,  "5",  false},
            {-3.0f,  "3",  false},
            { 0.0f,  "0",  true},
            { 3.0f,  "3",  true}
        };

        for (const auto& m : marks) {
            float angle = dbToAngle(m.db);
            float cosA = std::sin(angle);
            float sinA = -std::cos(angle);

            float innerR = radius - (m.isRed ? 7.5f : 5.5f);
            float outerR = radius + 2.5f;

            float x1 = pivotX + innerR * cosA;
            float y1 = pivotY + sinA * innerR;
            float x2 = pivotX + outerR * cosA;
            float y2 = pivotY + sinA * outerR;

            g.setColour(m.isRed ? juce::Colour(0xffcc2211) : juce::Colour(0xff22201d));
            g.drawLine(x1, y1, x2, y2, m.isRed ? 1.6f : 1.1f);

            float textR = radius - 14.5f;
            float tx = pivotX + textR * cosA;
            float ty = pivotY + sinA * textR;

            g.setFont(BoutiqueLookAndFeel::condensedFont(face.getHeight() * 0.17f, 0.02f));
            g.drawText(m.text, (int)tx - 12, (int)ty - 7, 24, 14, juce::Justification::centred, false);
        }

        g.setFont(BoutiqueLookAndFeel::condensedFont(face.getHeight() * 0.19f, 0.0f));
        g.setColour(juce::Colour(0xff22201d));
        g.drawText("-", (int)face.getX() + 5, (int)face.getY() + 5, 12, 12, juce::Justification::centred);
        g.setColour(juce::Colour(0xffcc2211));
        g.drawText("+", (int)face.getRight() - 22, (int)face.getY() + 5, 12, 12, juce::Justification::centred);

        // Engraved Bus VU Label
        g.setColour(juce::Colour(0xff2a2825).withAlpha(0.80f));
        g.setFont(BoutiqueLookAndFeel::condensedFont(face.getHeight() * 0.23f, 0.06f));
        g.drawText(label, (int)face.getX(), (int)face.getBottom() - (int)(face.getHeight() * 0.38f),
                   (int)face.getWidth(), 18, juce::Justification::centred, false);

        // 4. Peak Clip Indicator LED
        float ledX = face.getRight() - 13.0f;
        float ledY = face.getY() + 19.0f;
        float ledR = 3.5f;

        bool isPeaking = (currentLevelLinear > 0.99f || peakHoldCounter > 0);
        g.setColour(isPeaking ? juce::Colour(0xffff2200) : juce::Colour(0xff551100));
        g.fillEllipse(ledX - ledR, ledY - ledR, ledR * 2.0f, ledR * 2.0f);
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.drawEllipse(ledX - ledR, ledY - ledR, ledR * 2.0f, ledR * 2.0f, 1.0f);

        g.setFont(BoutiqueLookAndFeel::condensedFont(face.getHeight() * 0.13f, 0.04f));
        g.setColour(juce::Colour(0xff554433));
        g.drawText("PEAK", (int)ledX - 16, (int)ledY + 4, 32, 10, juce::Justification::centred, false);

        // 5. Dynamic Needle Pointer
        float currentAngle = dbToAngle(currentDb);
        float needleLen = radius - 2.0f;
        float nx = pivotX + needleLen * std::sin(currentAngle);
        float ny = pivotY - needleLen * std::cos(currentAngle);

        g.setColour(juce::Colours::black.withAlpha(0.16f));
        g.drawLine(pivotX + 1.5f, pivotY + 1.0f, nx + 1.5f, ny + 1.0f, 1.4f);

        g.setColour(juce::Colour(0xff181715));
        g.drawLine(pivotX, pivotY, nx, ny, 1.3f);

        // Bottom Pivot Housing Cap
        float capR = face.getHeight() * 0.14f;
        juce::Path capPath;
        capPath.addCentredArc(pivotX, pivotY, capR, capR, 0.0f,
                             juce::degreesToRadians(-90.0f), juce::degreesToRadians(90.0f), true);
        capPath.lineTo(pivotX, pivotY);
        capPath.closeSubPath();

        g.setColour(juce::Colour(0xff222226));
        g.fillPath(capPath);
        g.setColour(juce::Colour(0xff44444a));
        g.strokePath(capPath, juce::PathStrokeType(1.0f));
    }

private:
    void timerCallback() override {
        float targetLin = targetLevelLinear.load();

        if (targetLin > currentLevelLinear) {
            currentLevelLinear += (targetLin - currentLevelLinear) * 0.25f;
        } else {
            currentLevelLinear += (targetLin - currentLevelLinear) * 0.12f;
        }

        float db = (currentLevelLinear > 0.0001f) ? (20.0f * std::log10(currentLevelLinear)) : -40.0f;
        currentDb += (db - currentDb) * 0.3f;

        if (targetLin >= 0.99f) {
            peakHoldCounter = 15;
        } else if (peakHoldCounter > 0) {
            peakHoldCounter--;
        }

        repaint();
    }

    float dbToAngle(float db) const {
        float clamped = juce::jlimit(-20.0f, 3.0f, db);
        float norm = (clamped + 20.0f) / 23.0f;
        float startAngle = juce::degreesToRadians(-38.0f);
        float endAngle   = juce::degreesToRadians(38.0f);
        return startAngle + norm * (endAngle - startAngle);
    }

    juce::String label;
    std::atomic<float> targetLevelLinear{0.0f};
    float currentLevelLinear = 0.0f;
    float currentDb = -40.0f;
    int peakHoldCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AnalogVuMeter)
};
