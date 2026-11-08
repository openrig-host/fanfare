#pragma once

#include <JuceHeader.h>
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "FanfareConstants.h"
#include "FanfareSplashScreen.h"
#include "HelpLauncher.h"
#include "RigLibrary.h"
#include "ThemeManager.h"
#include "Version.h"

namespace Fanfare {

//==============================================================================
// Persists the Most-Recently-Used songs list in %APPDATA%/Fanfare/settings/recent_songs.json.
// On first launch (before recent_songs.json exists), pre-seeds from existing .json
// songs in RigLibrary::getSongsDirectory() sorted newest-first so existing rigs appear immediately.
class RecentSongs {
public:
    struct Entry {
        juce::File file;
        juce::int64 timestampMs = 0;
    };

    static juce::File storageFile() {
        return FanfareConstants::getSettingsDirectory().getChildFile("recent_songs.json");
    }

    static std::vector<Entry> load() {
        std::vector<Entry> out;
        const auto f = storageFile();
        if (f.existsAsFile()) {
            const auto parsed = juce::JSON::parse(f);
            if (const auto* arr = parsed.getArray()) {
                for (const auto& item : *arr) {
                    if (const auto* obj = item.getDynamicObject()) {
                        juce::File songFile(obj->getProperty("path").toString());
                        if (songFile.existsAsFile()) {
                            Entry e;
                            e.file = songFile;
                            e.timestampMs = (juce::int64)obj->getProperty("timeMs");
                            if (e.timestampMs <= 0)
                                e.timestampMs = songFile.getLastModificationTime().toMilliseconds();
                            out.push_back(std::move(e));
                        }
                    }
                }
            }
            return out;
        }

        // Seed from existing songs directory when recent_songs.json hasn't been written yet
        const auto songsDir = RigLibrary::getSongsDirectory();
        if (songsDir.isDirectory()) {
            for (const auto& iter : juce::RangedDirectoryIterator(
                     songsDir, true, "*.json", juce::File::findFiles)) {
                const auto sf = iter.getFile();
                Entry e;
                e.file = sf;
                e.timestampMs = sf.getLastModificationTime().toMilliseconds();
                out.push_back(std::move(e));
            }
            std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
                return a.timestampMs > b.timestampMs;
            });
            if (out.size() > kMaxEntries)
                out.resize(kMaxEntries);
        }
        return out;
    }

    static void record(const juce::File& file) {
        if (!file.existsAsFile())
            return;
        auto entries = load();
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) {
                          return e.file == file;
                      }),
                      entries.end());
        Entry latest;
        latest.file = file;
        latest.timestampMs = juce::Time::getCurrentTime().toMilliseconds();
        entries.insert(entries.begin(), std::move(latest));
        if (entries.size() > kMaxEntries)
            entries.resize(kMaxEntries);
        save(entries);
    }

    static void remove(const juce::File& file) {
        auto entries = load();
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) {
                          return e.file == file;
                      }),
                      entries.end());
        save(entries);
    }

    static juce::String formatRelativeTime(juce::int64 timeMs) {
        if (timeMs <= 0)
            return {};
        const juce::int64 nowMs = juce::Time::getCurrentTime().toMilliseconds();
        const juce::int64 diffSec = juce::jmax<juce::int64>(0, (nowMs - timeMs) / 1000);
        if (diffSec < 90)
            return "Just now";
        if (diffSec < 3600)
            return juce::String(diffSec / 60) + "m ago";
        if (diffSec < 86400)
            return juce::String(diffSec / 3600) + "h ago";
        if (diffSec < 172800)
            return "Yesterday";
        if (diffSec < 86400 * 7)
            return juce::String(diffSec / 86400) + "d ago";
        return juce::Time(timeMs).formatted("%b %d");
    }

private:
    static constexpr size_t kMaxEntries = 20;

    static void save(const std::vector<Entry>& entries) {
        juce::Array<juce::var> arr;
        for (const auto& e : entries) {
            juce::DynamicObject::Ptr obj = new juce::DynamicObject();
            obj->setProperty("path", e.file.getFullPathName());
            obj->setProperty("timeMs", e.timestampMs);
            arr.add(juce::var(obj.get()));
        }
        const auto f = storageFile();
        f.getParentDirectory().createDirectory();
        f.replaceWithText(juce::JSON::toString(juce::var(arr)));
    }
};

//==============================================================================
// Persists "Show on startup" toggle in ui_settings.json (default: true).
struct WelcomePrefs {
    static bool getShowOnStartup() {
        const auto f = FanfareConstants::getAppDirectory().getChildFile("ui_settings.json");
        if (!f.existsAsFile())
            return true;
        const auto v = juce::JSON::parse(f.loadFileAsString());
        if (v.isObject() && v.hasProperty("showWelcomeOnStartup"))
            return (bool)v["showWelcomeOnStartup"];
        return true;
    }

    static void setShowOnStartup(bool show) {
        auto f = FanfareConstants::getAppDirectory().getChildFile("ui_settings.json");
        juce::var v;
        if (f.existsAsFile())
            v = juce::JSON::parse(f.loadFileAsString());
        if (!v.isObject())
            v = juce::var(new juce::DynamicObject());
        if (auto* obj = v.getDynamicObject())
            obj->setProperty("showWelcomeOnStartup", show);
        f.getParentDirectory().createDirectory();
        f.replaceWithText(juce::JSON::toString(v));
    }
};

//==============================================================================
// Nota-inspired Startup / Welcome Modal Overlay.
class StartupWelcomeModal : public juce::Component, private juce::ListBoxModel {
public:
    struct Actions {
        std::function<void()> onNewSong;
        std::function<void()> onOpenSong;
        std::function<void()> onBuildSong;
        std::function<void()> onScanPlugins;
        std::function<void(const juce::File&)> onLoadRecentSong;
        std::function<void()> onOpenSettings;
        std::function<void()> onClose;
    };

    StartupWelcomeModal(Actions acts, int availablePluginCount)
        : actions(std::move(acts)), numPlugins(availablePluginCount) {
        setWantsKeyboardFocus(true);
        recentEntries = RecentSongs::load();

        // Top-right close 'x' button
        addAndMakeVisible(closeBtn);
        closeBtn.onClick = [this] { dismiss(); };

        // Primary action row
        newSongBtn.setButtonText("New song");
        openSongBtn.setButtonText("Open song...");
        buildSongBtn.setButtonText("Build from strips...");
        scanPluginsBtn.setButtonText("Scan VST3 plug-ins...");

        for (auto* b : {&newSongBtn, &openSongBtn, &buildSongBtn}) {
            setButtonRole(*b, ButtonRole::Standard);
            addAndMakeVisible(b);
        }

        setButtonRole(scanPluginsBtn, ButtonRole::Primary);
        scanPluginsBtn.setToggleState(true, juce::dontSendNotification);
        scanPluginsBtn.setVisible(numPlugins <= 0);
        addChildComponent(scanPluginsBtn);

        newSongBtn.onClick = [this] {
            auto cb = actions.onNewSong;
            dismiss();
            if (cb) cb();
        };
        openSongBtn.onClick = [this] {
            auto cb = actions.onOpenSong;
            dismiss();
            if (cb) cb();
        };
        buildSongBtn.onClick = [this] {
            auto cb = actions.onBuildSong;
            dismiss();
            if (cb) cb();
        };
        scanPluginsBtn.onClick = [this] {
            auto cb = actions.onScanPlugins;
            dismiss();
            if (cb) cb();
        };

        // Recent songs ListBox
        recentList.setModel(this);
        recentList.setRowHeight(34);
        recentList.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        recentList.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        recentList.addMouseListener(this, true);
        addAndMakeVisible(recentList);

        // Footer: Nota-style orange pill switch + quiet text links
        showOnStartupSwitch.setState(WelcomePrefs::getShowOnStartup());
        showOnStartupSwitch.onToggle = [](bool on) {
            WelcomePrefs::setShowOnStartup(on);
        };
        addAndMakeVisible(showOnStartupSwitch);

        docsLink.onClick = [] { openHelpManual("#main-mixer"); };
        settingsLink.onClick = [this] {
            auto cb = actions.onOpenSettings;
            dismiss();
            if (cb) cb();
        };
        whatsNewLink.onClick = [this] {
            showingWhatsNew = !showingWhatsNew;
            whatsNewLink.setText(showingWhatsNew ? "Recent songs" : "What's New");
            recentList.setVisible(!showingWhatsNew && !recentEntries.empty());
            resized();
            repaint();
        };
        addAndMakeVisible(docsLink);
        addAndMakeVisible(settingsLink);
        addAndMakeVisible(whatsNewLink);

        recentList.setVisible(!recentEntries.empty());
    }

    ~StartupWelcomeModal() override {
        recentList.removeMouseListener(this);
    }

    void paint(juce::Graphics& g) override {
        const auto& t = ThemeManager::getInstance().active();

        // Scrim
        g.fillAll(juce::Colours::black.withAlpha(0.50f));

        const auto card = cardBounds().toFloat();

        // Subtle drop shadow behind card
        juce::DropShadow(juce::Colours::black.withAlpha(0.55f), 26, {0, 8})
            .drawForRectangle(g, card.toNearestInt());

        // Card body
        const juce::Colour cardBg = t.isStudio() ? juce::Colour(0xFF242529) : t.panel;
        const juce::Colour cardBorder = t.isStudio() ? juce::Colour(0xFF3E4047) : t.borderStrong;
        g.setColour(cardBg);
        g.fillRoundedRectangle(card, 6.0f);
        g.setColour(cardBorder);
        g.drawRoundedRectangle(card.reduced(0.5f), 6.0f, 1.0f);

        // Brand badge + title + engine/VST subtitle
        const float padX = card.getX() + 24.0f;
        const float topY = card.getY() + 24.0f;
        drawBrandBadge(g, {padX, topY, 46.0f, 46.0f});

        g.setColour(t.text);
        g.setFont(BoutiqueLookAndFeel::uiFont(16.0f, true));
        g.drawText("Fanfare", (int)(padX + 60.0f), (int)(topY + 4.0f), 320, 20,
                   juce::Justification::centredLeft, false);

        const juce::String dot = juce::String::charToString(0x00B7);
        juce::String sub = "Version " OPENRIG_VERSION_STRING "  " + dot + "  Stage Engine  " + dot + "  ";
        sub += (numPlugins > 0) ? (juce::String(numPlugins) + " VST3 plug-ins")
                                : juce::String("VST3 Host");
        g.setColour(t.textDim);
        g.setFont(BoutiqueLookAndFeel::monoFont(11.5f, false));
        g.drawText(sub, (int)(padX + 60.0f), (int)(topY + 25.0f), 450, 18,
                   juce::Justification::centredLeft, true);

        // Section label above the recessed well
        const auto well = wellBounds();
        g.setColour(t.textDim);
        g.setFont(BoutiqueLookAndFeel::uiFont(12.0f, false));
        if (showingWhatsNew) {
            g.drawText("What's New in Fanfare " OPENRIG_VERSION_STRING,
                       well.getX(), well.getY() - 22, 320, 18,
                       juce::Justification::centredLeft, false);
        } else {
            const juce::String hdr = "Recent songs";
            g.drawText(hdr, well.getX(), well.getY() - 22, 90, 18,
                       juce::Justification::centredLeft, false);
            g.setColour(t.textFaint);
            g.setFont(BoutiqueLookAndFeel::monoFont(11.5f, false));
            g.drawText(juce::String((int)recentEntries.size()),
                       well.getX() + 80, well.getY() - 22, 48, 18,
                       juce::Justification::centredLeft, false);
        }

        // Recessed dark well (#111214)
        const juce::Colour wellBg = t.isStudio() ? juce::Colour(0xFF111214) : t.sunken;
        const juce::Colour wellBorder = t.isStudio() ? juce::Colour(0xFF32343B) : t.border;
        g.setColour(wellBg);
        g.fillRoundedRectangle(well.toFloat(), 2.0f);
        g.setColour(wellBorder);
        g.drawRoundedRectangle(well.toFloat().reduced(0.5f), 2.0f, 1.0f);

        if (showingWhatsNew) {
            paintWhatsNew(g, well.reduced(16, 14), t);
        } else if (recentEntries.empty()) {
            g.setColour(t.textFaint);
            g.setFont(BoutiqueLookAndFeel::uiFont(13.0f, false));
            g.drawText("No recent songs", well, juce::Justification::centred, false);
        }
    }

    void resized() override {
        const auto card = cardBounds();
        closeBtn.setBounds(card.getRight() - 36, card.getY() + 8, 28, 28);

        // Primary action row
        int bx = card.getX() + 24;
        const int by = card.getY() + 86;
        const int bh = 28;
        newSongBtn.setBounds(bx, by, 94, bh);
        bx += 94 + 8;
        openSongBtn.setBounds(bx, by, 108, bh);
        bx += 108 + 8;
        buildSongBtn.setBounds(bx, by, 134, bh);
        bx += 134 + 8;
        if (scanPluginsBtn.isVisible())
            scanPluginsBtn.setBounds(bx, by, 148, bh);

        // Recessed recent-songs well
        const auto well = wellBounds();
        recentList.setBounds(well.reduced(2));

        // Footer row
        const int fy = card.getBottom() - 36;
        showOnStartupSwitch.setBounds(card.getX() + 24, fy, 148, 22);

        int rx = card.getRight() - 24;
        const int whatsW = showingWhatsNew ? 86 : 74;
        rx -= whatsW;
        whatsNewLink.setBounds(rx, fy, whatsW, 22);
        rx -= 14 + 56;
        settingsLink.setBounds(rx, fy, 56, 22);
        rx -= 14 + 92;
        docsLink.setBounds(rx, fy, 92, 22);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (e.eventComponent == this && !cardBounds().contains(e.getPosition()))
            dismiss();
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (key == juce::KeyPress::escapeKey) {
            dismiss();
            return true;
        }
        return false;
    }

private:
    // Minimalist close 'x' button at top-right of modal card
    class CloseCrossButton : public juce::Button {
    public:
        CloseCrossButton() : juce::Button("Close") {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        }
        void paintButton(juce::Graphics& g, bool over, bool down) override {
            const auto& t = ThemeManager::getInstance().active();
            if (over || down) {
                g.setColour(t.raised.withAlpha(down ? 0.9f : 0.55f));
                g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f), 4.0f);
            }
            g.setColour((over || down) ? t.text : t.textDim);
            const auto c = getLocalBounds().toFloat().getCentre();
            const float d = 4.5f;
            g.drawLine(c.x - d, c.y - d, c.x + d, c.y + d, 1.4f);
            g.drawLine(c.x + d, c.y - d, c.x - d, c.y + d, 1.4f);
        }
    };

    // Nota-style pill toggle switch ("[o ] Show on startup")
    class PillToggleSwitch : public juce::Component {
    public:
        std::function<void(bool)> onToggle;

        PillToggleSwitch() {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        }

        void setState(bool on) {
            state = on;
            repaint();
        }

        void paint(juce::Graphics& g) override {
            const auto& t = ThemeManager::getInstance().active();
            const float cy = getHeight() * 0.5f;
            const juce::Rectangle<float> pill(0.0f, cy - 7.0f, 25.0f, 14.0f);

            g.setColour(state ? t.accent : (t.isStudio() ? juce::Colour(0xFF3A3C42) : t.raised));
            g.fillRoundedRectangle(pill, 7.0f);

            const float knobR = 4.6f;
            const float knobCx = state ? (pill.getRight() - 7.0f) : (pill.getX() + 7.0f);
            g.setColour(state ? juce::Colour(0xFF17181A) : t.textDim);
            g.fillEllipse(knobCx - knobR, cy - knobR, knobR * 2.0f, knobR * 2.0f);

            g.setColour(hover ? t.text : t.text.withAlpha(0.88f));
            g.setFont(BoutiqueLookAndFeel::uiFont(12.5f, false));
            g.drawText("Show on startup", 33, 0, getWidth() - 33, getHeight(),
                       juce::Justification::centredLeft, false);
        }

        void mouseEnter(const juce::MouseEvent&) override { hover = true; repaint(); }
        void mouseExit(const juce::MouseEvent&) override { hover = false; repaint(); }
        void mouseUp(const juce::MouseEvent& e) override {
            if (getLocalBounds().contains(e.getPosition())) {
                state = !state;
                repaint();
                if (onToggle)
                    onToggle(state);
            }
        }

    private:
        bool state = true;
        bool hover = false;
    };

    // Borderless footer text link ("Documentation", "Settings", "What's New")
    class FooterLink : public juce::Button {
    public:
        explicit FooterLink(const juce::String& label) : juce::Button(label) {
            setButtonText(label);
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        }
        void setText(const juce::String& label) {
            setButtonText(label);
            repaint();
        }
        void paintButton(juce::Graphics& g, bool over, bool down) override {
            const auto& t = ThemeManager::getInstance().active();
            g.setColour((over || down) ? t.accent : t.text.withAlpha(0.82f));
            g.setFont(BoutiqueLookAndFeel::uiFont(12.5f, true));
            g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centredRight, false);
        }
    };

    juce::Rectangle<int> cardBounds() const {
        const int w = juce::jmin(600, juce::jmax(420, getWidth() - 40));
        const int h = juce::jmin(428, juce::jmax(320, getHeight() - 40));
        return getLocalBounds().withSizeKeepingCentre(w, h);
    }

    juce::Rectangle<int> wellBounds() const {
        const auto card = cardBounds();
        const int top = card.getY() + 150;
        const int bottom = card.getBottom() - 50;
        return {card.getX() + 24, top, card.getWidth() - 48, juce::jmax(80, bottom - top)};
    }

    void dismiss() {
        if (actions.onClose)
            actions.onClose();
    }

    void paintWhatsNew(juce::Graphics& g, juce::Rectangle<int> area, const Theme& t) const {
        struct Bullet { const char* title; const char* detail; };
        static constexpr Bullet kItems[] = {
            {"Studio interface & vertical ribbon",
             "Calm neutral greys, single amber-orange focus accent, and a context-switching sidebar for Songs, Strips, Setlist, Queue, and MIDI."},
            {"Define-once Controller Profile",
             "Name your keyboard's physical knobs, drawbars, and pedals once - Fanfare labels those CCs everywhere and applies them in one click."},
            {"Automatic Save Version History",
             "Every saved song, strip, and queue map retains timestamped snapshots you can inspect or restore from right-click -> Version history."},
            {"Stage-hardened v1.41 audio engine",
             "Parallel VST3 rig loading, hybrid P-core DSP isolation, automatic USB audio/MIDI hotplug recovery, and live thermal monitoring."},
        };

        int y = area.getY();
        for (const auto& item : kItems) {
            g.setColour(t.accent);
            g.fillEllipse((float)area.getX(), (float)y + 6.0f, 5.0f, 5.0f);

            g.setColour(t.text);
            g.setFont(BoutiqueLookAndFeel::uiFont(12.5f, true));
            g.drawText(item.title, area.getX() + 12, y, area.getWidth() - 12, 16,
                       juce::Justification::centredLeft, true);
            y += 17;

            g.setColour(t.textDim);
            g.setFont(BoutiqueLookAndFeel::uiFont(11.5f, false));
            g.drawFittedText(item.detail, area.getX() + 12, y, area.getWidth() - 12, 28,
                             juce::Justification::topLeft, 2);
            y += 30;
        }
    }

    // ListBoxModel for Recent Songs
    int getNumRows() override { return (int)recentEntries.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override {
        if (row < 0 || row >= (int)recentEntries.size())
            return;
        const auto& t = ThemeManager::getInstance().active();
        const auto& entry = recentEntries[(size_t)row];
        const bool hovered = (row == hoveredRow);

        if (selected || hovered) {
            g.setColour(t.isStudio() ? juce::Colour(0xFF1B1D22) : t.panelAlt);
            g.fillRect(0, 0, width, height);
            g.setColour(t.accent);
            g.fillRect(0, 4, 2, height - 8);
        }

        const juce::String songName = entry.file.getFileNameWithoutExtension();
        const juce::File songsRoot = RigLibrary::getSongsDirectory();
        juce::String folderHint;
        if (entry.file.getParentDirectory() != songsRoot)
            folderHint = entry.file.getParentDirectory().getFileName();

        const juce::String relTime = RecentSongs::formatRelativeTime(entry.timestampMs);

        // Right-aligned mono relative timestamp
        g.setColour(t.textFaint);
        g.setFont(BoutiqueLookAndFeel::monoFont(11.0f, false));
        g.drawText(relTime, width - 98, 0, 86, height, juce::Justification::centredRight, false);

        // Song title + optional folder tag
        g.setColour(t.text);
        g.setFont(BoutiqueLookAndFeel::uiFont(13.0f, true));
        const int nameMaxW = width - 118;
        g.drawText(songName, 12, 0, nameMaxW, height, juce::Justification::centredLeft, true);

        if (folderHint.isNotEmpty()) {
            juce::GlyphArrangement ga;
            ga.addLineOfText(BoutiqueLookAndFeel::uiFont(13.0f, true), songName, 0.0f, 0.0f);
            const int titleW = juce::roundToInt(ga.getBoundingBox(0, -1, true).getWidth());
            if (titleW + 24 < nameMaxW) {
                g.setColour(t.textDim);
                g.setFont(BoutiqueLookAndFeel::uiFont(11.5f, false));
                g.drawText(juce::String::charToString(0x00B7) + "  " + folderHint,
                           18 + titleW, 0, nameMaxW - titleW - 12, height,
                           juce::Justification::centredLeft, true);
            }
        }

        g.setColour(t.hairline.withAlpha(0.45f));
        g.drawHorizontalLine(height - 1, 10.0f, (float)(width - 10));
    }

    void listBoxItemClicked(int row, const juce::MouseEvent& e) override {
        if (row < 0 || row >= (int)recentEntries.size())
            return;
        const auto file = recentEntries[(size_t)row].file;
        if (e.mods.isPopupMenu()) {
            juce::PopupMenu m;
            m.addItem(1, "Load song");
            m.addItem(2, "Reveal in Explorer");
            m.addSeparator();
            m.addItem(3, "Remove from recent list");
            juce::Component::SafePointer<StartupWelcomeModal> safeThis(this);
            m.showMenuAsync(juce::PopupMenu::Options(), [safeThis, file](int r) {
                if (safeThis == nullptr)
                    return;
                if (r == 1) {
                    auto cb = safeThis->actions.onLoadRecentSong;
                    safeThis->dismiss();
                    if (cb) cb(file);
                } else if (r == 2) {
                    file.revealToUser();
                } else if (r == 3) {
                    RecentSongs::remove(file);
                    safeThis->recentEntries = RecentSongs::load();
                    safeThis->recentList.updateContent();
                    safeThis->recentList.setVisible(!safeThis->recentEntries.empty());
                    safeThis->repaint();
                }
            });
            return;
        }

        // Single click loads the song right away (fast workflow)
        auto cb = actions.onLoadRecentSong;
        dismiss();
        if (cb)
            cb(file);
    }

    void mouseMove(const juce::MouseEvent& e) override {
        const auto selfPt = e.getEventRelativeTo(this).getPosition();
        const auto rel = e.getEventRelativeTo(&recentList).getPosition();
        const int r = recentList.getBounds().contains(selfPt)
                          ? recentList.getRowContainingPosition(rel.x, rel.y)
                          : -1;
        if (r != hoveredRow) {
            hoveredRow = r;
            recentList.repaint();
        }
    }

    void mouseExit(const juce::MouseEvent& e) override {
        const auto selfPt = e.getEventRelativeTo(this).getPosition();
        if (!recentList.getBounds().contains(selfPt) && hoveredRow != -1) {
            hoveredRow = -1;
            recentList.repaint();
        }
    }

    Actions actions;
    int numPlugins = 0;
    bool showingWhatsNew = false;
    int hoveredRow = -1;
    std::vector<RecentSongs::Entry> recentEntries;

    CloseCrossButton closeBtn;
    juce::TextButton newSongBtn, openSongBtn, buildSongBtn, scanPluginsBtn;
    juce::ListBox recentList;
    PillToggleSwitch showOnStartupSwitch;
    FooterLink docsLink{"Documentation"};
    FooterLink settingsLink{"Settings"};
    FooterLink whatsNewLink{"What's New"};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StartupWelcomeModal)
};

} // namespace Fanfare
