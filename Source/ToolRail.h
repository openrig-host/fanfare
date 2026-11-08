#pragma once

#include <JuceHeader.h>
#include "ThemeManager.h"

// ToolRail - Nota-style vertical icon rail ("a vertical ribbon").
//
// The top group switches the context sidebar beside it (Songs, Strips,
// Setlist, Queue, MIDI monitor): click an icon to show that context, click
// the active icon again to collapse the sidebar. The middle group opens the
// windowed tools; Help and Settings sit at the bottom. Every icon is a real
// child component, so it gets a tooltip and a hover state. Vector glyphs in
// theme colours; the active context gets the accent edge marker.
//
// Ported from the GLM revamp (glm-nota-revamp branch) and extended to own all
// sidebar contexts, replacing the library's tab strip.

namespace Fanfare {

class ToolRail : public juce::Component {
public:
    enum Tool {
        Songs = 0, Strips, Setlist, Queue, Midi,   // sidebar contexts
        Notepad, Inspector, Remote,                // windowed tools
        Help, Settings,                            // bottom group
        NumTools
    };
    static constexpr int kNumContexts = Midi + 1;

    std::function<void(int)> onTool;

    ToolRail() {
        static const char* const tips[NumTools] = {
            "Songs - your song library (click again to close the sidebar)",
            "Strips - saved channel strip presets",
            "Setlist - running order for tonight",
            "Queue - the 10 one-touch song tiles and saved queue maps",
            "MIDI monitor - live MIDI event log",
            "Gig notepad - chord charts and set notes (Ctrl+N)",
            "Resource inspector - per-plugin CPU and RAM",
            "Tablet remote - Stage HUD and phone hotspot",
            "Help - open the user manual (F1)",
            "Settings - audio, MIDI, theme and engine",
        };
        for (int i = 0; i < NumTools; ++i) {
            buttons[i] = std::make_unique<RailButton>(i);
            buttons[i]->setTooltip(tips[i]);
            buttons[i]->onClick = [this](int t) {
                if (onTool)
                    onTool(t);
            };
            addAndMakeVisible(*buttons[i]);
        }
    }

    // -1 = no context open
    void setActiveContext(int ctx) {
        for (int i = 0; i < NumTools; ++i)
            buttons[i]->active = (i == ctx);
        repaint();
        for (auto& b : buttons)
            b->repaint();
    }

    // Small attention dot (e.g. MIDI activity, update available)
    void setBadge(int tool, bool on) {
        if (tool >= 0 && tool < NumTools && buttons[tool]->badge != on) {
            buttons[tool]->badge = on;
            buttons[tool]->repaint();
        }
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(ThemeManager::get(Theme::Role::background));
        g.setColour(ThemeManager::get(Theme::Role::border));
        g.drawVerticalLine(getWidth() - 1, 0.0f, (float)getHeight());
        // separator between sidebar contexts and windowed tools
        const float y = rowHeight * (float)kNumContexts + 4.0f;
        g.drawHorizontalLine((int)y, 10.0f, (float)getWidth() - 10.0f);
    }

    void resized() override {
        const int w = getWidth();
        const int rh = (int)rowHeight;
        int y = 0;
        for (int i = 0; i < kNumContexts; ++i, y += rh)
            buttons[i]->setBounds(0, y, w, rh);
        y += 9;
        for (int i = Notepad; i <= Remote; ++i, y += rh)
            buttons[i]->setBounds(0, y, w, rh);
        int by = getHeight() - 2 * rh - 4;
        buttons[Help]->setBounds(0, by, w, rh);
        buttons[Settings]->setBounds(0, by + rh, w, rh);
    }

private:
    static constexpr float rowHeight = 42.0f;

    struct RailButton : public juce::Component, public juce::SettableTooltipClient {
        int tool = 0;
        bool active = false;
        bool badge = false;
        std::function<void(int)> onClick;

        explicit RailButton(int t) : tool(t) {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        }

        void paint(juce::Graphics& g) override {
            auto row = getLocalBounds().toFloat();
            const bool over = isMouseOver();
            if (active) {
                g.setColour(ThemeManager::get(Theme::Role::panel));
                g.fillRect(row);
                g.setColour(ThemeManager::get(Theme::Role::accent));
                g.fillRect(row.withWidth(2.0f).reduced(0.0f, 9.0f));
            } else if (over) {
                g.setColour(ThemeManager::get(Theme::Role::panelAlt).withAlpha(0.7f));
                g.fillRect(row);
            }
            const auto col = active ? ThemeManager::get(Theme::Role::accent)
                                    : (over ? ThemeManager::get(Theme::Role::text)
                                            : ThemeManager::get(Theme::Role::textDim));
            drawGlyph(g, tool, row.getCentre(), col, 18.0f);
            if (badge) {
                g.setColour(ThemeManager::get(Theme::Role::accent));
                g.fillEllipse(row.getCentreX() + 7.0f, row.getCentreY() - 11.0f, 6.0f, 6.0f);
            }
        }
        void mouseUp(const juce::MouseEvent& e) override {
            if (e.mouseWasClicked() && getLocalBounds().contains(e.getPosition()) && onClick)
                onClick(tool);
        }
        void mouseEnter(const juce::MouseEvent&) override { repaint(); }
        void mouseExit(const juce::MouseEvent&) override { repaint(); }
    };

    static void drawGlyph(juce::Graphics& g, int tool, juce::Point<float> c,
                          juce::Colour col, float s) {
        g.setColour(col);
        const auto r = juce::Rectangle<float>(s, s).withCentre(c);
        const juce::PathStrokeType stroke(1.5f, juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded);
        juce::Path p;
        switch (tool) {
        case Songs: { // eighth note
            const float hx = r.getX() + s * 0.30f, hy = r.getBottom() - s * 0.22f;
            g.fillEllipse(hx - s * 0.17f, hy - s * 0.12f, s * 0.34f, s * 0.25f);
            const float stemX = hx + s * 0.15f;
            p.startNewSubPath(stemX, hy);
            p.lineTo(stemX, r.getY() + s * 0.08f);
            p.quadraticTo(stemX + s * 0.38f, r.getY() + s * 0.20f, stemX + s * 0.36f, r.getY() + s * 0.48f);
            g.strokePath(p, stroke);
            return;
        }
        case Strips: { // three faders
            for (int i = 0; i < 3; ++i) {
                const float x = r.getX() + s * (0.2f + 0.3f * (float)i);
                g.drawLine(x, r.getY() + 1.0f, x, r.getBottom() - 1.0f, 1.4f);
                const float ky = r.getY() + s * (i == 0 ? 0.62f : (i == 1 ? 0.30f : 0.50f));
                g.fillRoundedRectangle(x - 3.5f, ky - 2.0f, 7.0f, 4.0f, 1.0f);
            }
            return;
        }
        case Setlist: { // numbered list
            for (int i = 0; i < 3; ++i) {
                const float y = r.getY() + s * (0.2f + 0.3f * (float)i);
                g.fillEllipse(r.getX() + 1.0f, y - 1.6f, 3.2f, 3.2f);
                g.drawLine(r.getX() + 7.0f, y, r.getRight() - 1.0f, y, 1.5f);
            }
            return;
        }
        case Queue: { // 2x2 tiles
            const float gap = 3.0f, tile = (s - gap) * 0.5f;
            for (int i = 0; i < 4; ++i)
                g.drawRoundedRectangle(r.getX() + (float)(i % 2) * (tile + gap) + 0.5f,
                                       r.getY() + (float)(i / 2) * (tile + gap) + 0.5f,
                                       tile - 1.0f, tile - 1.0f, 2.0f, 1.4f);
            return;
        }
        case Midi: { // 5-pin DIN
            g.drawEllipse(r.reduced(1.0f), 1.4f);
            const float rr = s * 0.27f;
            for (int i = 0; i < 5; ++i) {
                const float a = juce::degreesToRadians(180.0f + 45.0f * (float)i);
                g.fillEllipse(c.x + rr * std::cos(a) - 1.4f, c.y + rr * std::sin(a) * -1.0f - 1.4f + 1.0f, 2.8f, 2.8f);
            }
            g.fillRect(c.x - 2.0f, r.getBottom() - 4.0f, 4.0f, 2.0f);
            return;
        }
        case Notepad: { // page with lines
            g.drawRoundedRectangle(r.reduced(2.5f, 0.5f), 2.0f, 1.4f);
            for (int i = 0; i < 3; ++i)
                g.drawLine(r.getX() + 6.0f, r.getY() + s * (0.32f + 0.2f * (float)i),
                           r.getRight() - 6.0f, r.getY() + s * (0.32f + 0.2f * (float)i), 1.2f);
            return;
        }
        case Inspector: { // gauge dial + needle
            p.addCentredArc(c.x, c.y + 2.0f, s * 0.45f, s * 0.45f, 0.0f,
                            juce::degreesToRadians(-120.0f), juce::degreesToRadians(120.0f), true);
            g.strokePath(p, stroke);
            g.drawLine(c.x, c.y + 2.0f, c.x + s * 0.24f, c.y - s * 0.18f, 1.6f);
            g.fillEllipse(c.x - 2.0f, c.y, 4.0f, 4.0f);
            return;
        }
        case Remote: { // wifi fan
            const float cx = c.x, cy = c.y + s * 0.28f;
            for (int i = 0; i < 2; ++i) {
                const float rad = s * (0.28f + 0.22f * (float)i);
                juce::Path arc;
                arc.addCentredArc(cx, cy, rad, rad, 0.0f, juce::degreesToRadians(-45.0f),
                                  juce::degreesToRadians(45.0f), true);
                g.strokePath(arc, stroke);
            }
            g.fillEllipse(cx - 2.0f, cy - 2.0f, 4.0f, 4.0f);
            return;
        }
        case Help: { // circled ?
            g.drawEllipse(r.reduced(1.0f), 1.4f);
            g.setFont(juce::FontOptions(s * 0.68f, juce::Font::bold));
            g.drawText("?", r, juce::Justification::centred, false);
            return;
        }
        case Settings: { // gear
            const float ro = s * 0.46f, ri = s * 0.33f;
            for (int i = 0; i < 16; ++i) {
                const float a = juce::MathConstants<float>::twoPi * (float)i / 16.0f;
                const float rad = (i % 2 == 0) ? ro : ri;
                const auto pt = c.getPointOnCircumference(rad, a);
                if (i == 0) p.startNewSubPath(pt); else p.lineTo(pt);
            }
            p.closeSubPath();
            g.strokePath(p, juce::PathStrokeType(1.3f));
            g.drawEllipse(c.x - s * 0.13f, c.y - s * 0.13f, s * 0.26f, s * 0.26f, 1.3f);
            return;
        }
        default:
            return;
        }
    }

    std::unique_ptr<RailButton> buttons[NumTools];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToolRail)
};

} // namespace Fanfare

