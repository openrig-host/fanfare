#pragma once

#include <JuceHeader.h>
#include "ButtonRoles.h"
#include "FanfareEngine.h"
#include "HelpLauncher.h"
#include "ThemeManager.h"

class StageLiveNotesViewer : public juce::Component {
public:
    StageLiveNotesViewer(FanfareEngine& eng) : engine(eng) {
        setOpaque(true);
    }

    void setContent(const juce::String& text) {
        rawText = text;
        updateContentHeight();
        repaint();
    }

    void setMonospace(bool mono) {
        isMonospace = mono;
        updateContentHeight();
        repaint();
    }

    void setFontSize(float sz) {
        fontSize = sz;
        updateContentHeight();
        repaint();
    }

    struct Token {
        enum Type { NormalText, ColoredText, ChordBadge };
        Type type = NormalText;
        juce::String text;
        juce::Colour colour;
        bool isBold = false;
    };

    struct ParsedLine {
        std::vector<Token> tokens;
        bool isHeading = false;
        int headingLevel = 0;
        juce::String headingText;
    };

    std::vector<ParsedLine> parseLines() const {
        std::vector<ParsedLine> result;
        auto lines = juce::StringArray::fromLines(rawText);

        const auto textCol    = ThemeManager::get(Theme::Role::text);
        const auto brightCol  = ThemeManager::get(Theme::Role::text);
        const auto accentCol  = ThemeManager::get(Theme::Role::accent);
        const auto okCol      = ThemeManager::get(Theme::Role::ok);
        const auto fohCol     = ThemeManager::get(Theme::Role::foh);
        const auto dangerCol  = ThemeManager::get(Theme::Role::danger);
        const auto synthCol   = ThemeManager::get(Theme::Role::catKeyboard);

        for (auto line : lines) {
            ParsedLine pl;
            auto trimmed = line.trim();

            if (trimmed.startsWith("### ")) {
                pl.isHeading = true;
                pl.headingLevel = 3;
                pl.headingText = trimmed.substring(4);
                result.push_back(pl);
                continue;
            } else if (trimmed.startsWith("## ")) {
                pl.isHeading = true;
                pl.headingLevel = 2;
                pl.headingText = trimmed.substring(3);
                result.push_back(pl);
                continue;
            } else if (trimmed.startsWith("# ")) {
                pl.isHeading = true;
                pl.headingLevel = 1;
                pl.headingText = trimmed.substring(2);
                result.push_back(pl);
                continue;
            }

            int idx = 0;
            int len = line.length();
            juce::String curPlain;

            auto flushPlain = [&]() {
                if (curPlain.isNotEmpty()) {
                    Token t;
                    t.type = Token::NormalText;
                    t.text = curPlain;
                    t.colour = textCol;
                    pl.tokens.push_back(t);
                    curPlain.clear();
                }
            };

            while (idx < len) {
                if (idx + 1 < len && line[idx] == '*' && line[idx + 1] == '*') {
                    int endBold = line.indexOf(idx + 2, "**");
                    if (endBold > idx + 1) {
                        flushPlain();
                        Token t;
                        t.type = Token::ColoredText;
                        t.text = line.substring(idx + 2, endBold);
                        t.colour = brightCol;
                        t.isBold = true;
                        pl.tokens.push_back(t);
                        idx = endBold + 2;
                        continue;
                    }
                }

                if (line[idx] == '[') {
                    int closeBracket = line.indexOf(idx, "]");
                    if (closeBracket > idx) {
                        juce::String tagContent = line.substring(idx + 1, closeBracket).trim();
                        juce::String tagLower = tagContent.toLowerCase();

                        juce::Colour tagCol = juce::Colours::transparentBlack;
                        if (tagLower == "blue" || tagLower == "cyan") tagCol = accentCol;
                        else if (tagLower == "green") tagCol = okCol;
                        else if (tagLower == "yellow" || tagLower == "amber") tagCol = fohCol;
                        else if (tagLower == "white") tagCol = brightCol;
                        else if (tagLower == "red") tagCol = dangerCol;
                        else if (tagLower == "purple" || tagLower == "magenta") tagCol = synthCol;

                        if (!tagCol.isTransparent()) {
                            juce::String closeTagStr = "[/" + tagLower + "]";
                            int endTag = line.indexOfIgnoreCase(closeBracket + 1, closeTagStr);
                            if (endTag > closeBracket) {
                                flushPlain();
                                Token t;
                                t.type = Token::ColoredText;
                                t.text = line.substring(closeBracket + 1, endTag);
                                t.colour = tagCol;
                                t.isBold = true;
                                pl.tokens.push_back(t);
                                idx = endTag + closeTagStr.length();
                                continue;
                            }
                        }

                        if (tagLower == "chord") {
                            int endTag = line.indexOfIgnoreCase(closeBracket + 1, "[/chord]");
                            if (endTag > closeBracket) {
                                flushPlain();
                                Token t;
                                t.type = Token::ChordBadge;
                                t.text = line.substring(closeBracket + 1, endTag);
                                t.colour = accentCol;
                                pl.tokens.push_back(t);
                                idx = endTag + 8;
                                continue;
                            }
                        } else if (isLikelyChord(tagContent)) {
                            flushPlain();
                            Token t;
                            t.type = Token::ChordBadge;
                            t.text = tagContent;
                            t.colour = accentCol;
                            pl.tokens.push_back(t);
                            idx = closeBracket + 1;
                            continue;
                        }
                    }
                }

                curPlain += juce::String::charToString(line[idx]);
                idx++;
            }
            flushPlain();
            result.push_back(pl);
        }
        return result;
    }

    static bool isLikelyChord(const juce::String& s) {
        if (s.isEmpty() || s.length() > 14) return false;
        juce::juce_wchar root = s[0];
        if (root < 'A' || root > 'G') return false;
        return true;
    }

    void updateContentHeight() {
        auto parsed = parseLines();
        float sz = fontSize > 0 ? fontSize : 22.0f;
        float lineH = sz * 1.45f;
        int totalH = 40;

        for (const auto& pl : parsed) {
            if (pl.isHeading) totalH += (int)(lineH * 1.5f);
            else totalH += (int)lineH;
        }

        int parentH = getParentComponent() ? getParentComponent()->getHeight() : 400;
        setSize(juce::jmax(200, getWidth()), juce::jmax(parentH, totalH + 60));
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(ThemeManager::get(Theme::Role::sunken));

        if (rawText.trim().isEmpty()) {
            g.setColour(ThemeManager::get(Theme::Role::textDim));
            g.setFont(juce::FontOptions(fontSize > 0 ? fontSize : 20.0f));
            g.drawText("No notes or chord charts for this song. Switch to EDIT MODE to type or import notes.",
                       getLocalBounds().reduced(20), juce::Justification::centredTop, true);
            return;
        }

        float sz = fontSize > 0 ? fontSize : 22.0f;
        float lineH = sz * 1.45f;
        float curY = 16.0f;

        juce::Font baseFont = isMonospace
            ? juce::Font(juce::FontOptions().withName(juce::Font::getDefaultMonospacedFontName()).withHeight(sz))
            : juce::Font(juce::FontOptions(sz));

        const auto accentCol = ThemeManager::get(Theme::Role::accent);
        const auto fohCol    = ThemeManager::get(Theme::Role::foh);
        const auto okCol     = ThemeManager::get(Theme::Role::ok);

        auto parsed = parseLines();

        for (const auto& pl : parsed) {
            if (pl.isHeading) {
                float hSize = pl.headingLevel == 1 ? sz * 1.35f : (pl.headingLevel == 2 ? sz * 1.2f : sz * 1.1f);
                juce::Colour hCol = pl.headingLevel == 1 ? accentCol : (pl.headingLevel == 2 ? fohCol : okCol);

                juce::Font hFont = baseFont.withHeight(hSize).boldened();
                g.setFont(hFont);
                g.setColour(hCol);
                g.drawText(pl.headingText, 20, (int)curY, getWidth() - 40, (int)(hSize * 1.2f), juce::Justification::left);

                if (pl.headingLevel == 1) {
                    g.setColour(hCol.withAlpha(0.35f));
                    g.drawLine(20.0f, curY + hSize * 1.2f + 2.0f, (float)getWidth() - 20.0f, curY + hSize * 1.2f + 2.0f, 1.0f);
                }

                curY += lineH * 1.5f;
                continue;
            }

            if (pl.tokens.empty()) {
                curY += lineH;
                continue;
            }

            float curX = 20.0f;
            for (const auto& tok : pl.tokens) {
                if (tok.type == Token::ChordBadge) {
                    juce::Font cFont = baseFont.withHeight(sz).boldened();
                    g.setFont(cFont);
                    float strW = (float)cFont.getStringWidth(tok.text);
                    float badgeW = strW + 12.0f;
                    float badgeH = sz * 1.2f;

                    juce::Rectangle<float> badgeRect(curX, curY + (lineH - badgeH) * 0.5f, badgeW, badgeH);
                    g.setColour(accentCol.withAlpha(0.16f));
                    g.fillRoundedRectangle(badgeRect, 3.0f);
                    g.setColour(accentCol.withAlpha(0.60f));
                    g.drawRoundedRectangle(badgeRect, 3.0f, 1.0f);

                    g.setColour(accentCol);
                    g.drawText(tok.text, badgeRect, juce::Justification::centred, false);
                    curX += badgeW + 4.0f;
                } else {
                    juce::Font tFont = tok.isBold ? baseFont.boldened() : baseFont;
                    g.setFont(tFont);
                    g.setColour(tok.colour);

                    float strW = (float)tFont.getStringWidth(tok.text);
                    g.drawText(tok.text, (int)curX, (int)curY, (int)strW + 2, (int)lineH, juce::Justification::left, false);
                    curX += strW;
                }
            }
            curY += lineH;
        }
    }

private:
    FanfareEngine& engine;
    juce::String rawText;
    bool isMonospace = true;
    float fontSize = 22.0f;
};

class GigNotepadComponent : public juce::Component,
                            public juce::TextEditor::Listener {
public:
    GigNotepadComponent(FanfareEngine& eng) : engine(eng) {
        setOpaque(true);

        songTitleLabel.setFont(juce::FontOptions("Barlow Condensed", 15.0f, juce::Font::bold));
        songTitleLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::engrave));
        songTitleLabel.setText("GIG NOTEPAD", juce::dontSendNotification);
        addAndMakeVisible(songTitleLabel);

        // Live Mode Lock Toggle
        liveModeBtn.setButtonText("LIVE LOCKED");
        liveModeBtn.setClickingTogglesState(true);
        liveModeBtn.setToggleState(true, juce::dontSendNotification);
        Fanfare::setButtonRole(liveModeBtn, Fanfare::ButtonRole::Primary);
        liveModeBtn.onClick = [this] {
            updateLiveEditModeState();
        };
        addAndMakeVisible(liveModeBtn);
        Fanfare::styleHelpButton(helpBtn, "#gig-notepad", "HAAALP! Open the Gig Notepad & Chord Charts section of the User's Manual");
        addAndMakeVisible(helpBtn);

        zoomOutBtn.setButtonText("A-");
        Fanfare::setButtonRole(zoomOutBtn, Fanfare::ButtonRole::Standard);
        zoomOutBtn.onClick = [this] { changeFontSize(-2.0f); };
        addAndMakeVisible(zoomOutBtn);

        zoomInBtn.setButtonText("A+");
        Fanfare::setButtonRole(zoomInBtn, Fanfare::ButtonRole::Standard);
        zoomInBtn.onClick = [this] { changeFontSize(2.0f); };
        addAndMakeVisible(zoomInBtn);

        fontSizeLabel.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold));
        fontSizeLabel.setJustificationType(juce::Justification::centred);
        fontSizeLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::textDim));
        addAndMakeVisible(fontSizeLabel);

        fontTypeBtn.setButtonText("MONO");
        Fanfare::setButtonRole(fontTypeBtn, Fanfare::ButtonRole::Standard);
        fontTypeBtn.onClick = [this] {
            bool isMono = !engine.getNoteIsMonospace();
            engine.setNoteIsMonospace(isMono);
            fontTypeBtn.setButtonText(isMono ? "MONO" : "SANS");
            applyEditorFont();
        };
        addAndMakeVisible(fontTypeBtn);

        // Format Toolbar Buttons
        auto styleFmtBtn = [this](juce::TextButton& b, const juce::String& label, const juce::String& tag) {
            b.setButtonText(label);
            Fanfare::setButtonRole(b, Fanfare::ButtonRole::Standard);
            b.onClick = [this, tag] { insertFormatTag(tag); };
            addAndMakeVisible(b);
        };
        styleFmtBtn(btnBold,   "B",       "bold");
        styleFmtBtn(btnBlue,   "BLUE",    "blue");
        styleFmtBtn(btnGreen,  "GREEN",   "green");
        styleFmtBtn(btnYellow, "AMBER",   "yellow");
        styleFmtBtn(btnWhite,  "HI-TEXT", "white");
        styleFmtBtn(btnRed,    "RED",     "red");
        styleFmtBtn(btnChord,  "[CHORD]", "chord");
        Fanfare::setButtonRole(btnChord, Fanfare::ButtonRole::Primary);

        importBtn.setButtonText("IMPORT");
        Fanfare::setButtonRole(importBtn, Fanfare::ButtonRole::Standard);
        importBtn.onClick = [this] { importFilePrompt(); };
        addAndMakeVisible(importBtn);

        addTabBtn.setButtonText("+ TAB");
        Fanfare::setButtonRole(addTabBtn, Fanfare::ButtonRole::Standard);
        addTabBtn.onClick = [this] { addNewTabPrompt(); };
        addAndMakeVisible(addTabBtn);

        manageTabsBtn.setButtonText("TABS...");
        Fanfare::setButtonRole(manageTabsBtn, Fanfare::ButtonRole::Standard);
        manageTabsBtn.onClick = [this] { showTabManagementMenu(); };
        addAndMakeVisible(manageTabsBtn);

        // Configure Live Stage Viewer
        liveViewer = std::make_unique<StageLiveNotesViewer>(engine);
        liveViewport.setViewedComponent(liveViewer.get(), false);
        liveViewport.setScrollBarsShown(true, false);
        addAndMakeVisible(liveViewport);

        // Configure Main Text Editor
        textEditor.setMultiLine(true);
        textEditor.setReturnKeyStartsNewLine(true);
        textEditor.setTabKeyUsedAsCharacter(true);
        textEditor.setScrollbarsShown(true);
        textEditor.addListener(this);
        textEditor.setColour(juce::TextEditor::backgroundColourId, ThemeManager::get(Theme::Role::sunken));
        textEditor.setColour(juce::TextEditor::textColourId, ThemeManager::get(Theme::Role::text));
        textEditor.setColour(juce::TextEditor::outlineColourId, ThemeManager::get(Theme::Role::border));
        textEditor.setColour(juce::TextEditor::focusedOutlineColourId, ThemeManager::get(Theme::Role::accent));
        textEditor.setColour(juce::TextEditor::highlightColourId, ThemeManager::get(Theme::Role::accent).withAlpha(0.30f));
        addChildComponent(textEditor);

        loadFromEngine();
        updateLiveEditModeState();

        engine.addNotesListener(this, [this] {
            loadFromEngine();
        });
        lookAndFeelChanged();
    }

    void lookAndFeelChanged() override {
        const auto& t = ThemeManager::getInstance().active();
        songTitleLabel.setColour(juce::Label::textColourId, t.engrave);
        fontSizeLabel.setColour(juce::Label::textColourId, t.textDim);
        textEditor.setColour(juce::TextEditor::backgroundColourId, t.dark ? t.sunken : t.raised);
        textEditor.setColour(juce::TextEditor::textColourId, t.text);
        textEditor.setColour(juce::TextEditor::outlineColourId, t.dark ? t.border : t.borderStrong);
        textEditor.setColour(juce::TextEditor::focusedOutlineColourId, t.accent);
        textEditor.setColour(juce::TextEditor::highlightColourId, t.accent.withAlpha(0.30f));
        textEditor.applyColourToAllText(t.text, true);
        if (liveViewer) liveViewer->repaint();
    }

    void insertFormatTag(const juce::String& tag) {
        auto sel = textEditor.getHighlightedText();
        juce::String openTag = "[" + tag + "]";
        juce::String closeTag = "[/" + tag + "]";
        juce::String def = sel.isNotEmpty() ? sel : "text";

        if (tag == "bold") {
            openTag = "**";
            closeTag = "**";
            if (sel.isEmpty()) def = "bold text";
        } else if (tag == "chord") {
            openTag = "[";
            closeTag = "]";
            if (sel.isEmpty()) def = "C#m7";
        }

        auto fullText = openTag + def + closeTag;
        textEditor.insertTextAtCaret(fullText);
        saveCurrentTabContent();
        if (liveViewer) liveViewer->setContent(textEditor.getText());
    }

    void updateLiveEditModeState() {
        bool isLive = liveModeBtn.getToggleState();
        liveModeBtn.setButtonText(isLive ? "LIVE LOCKED" : "EDIT MODE");
        Fanfare::setButtonRole(liveModeBtn, isLive ? Fanfare::ButtonRole::Primary : Fanfare::ButtonRole::Foh);
        liveModeBtn.repaint();

        liveViewport.setVisible(isLive);
        textEditor.setVisible(!isLive);

        btnBold.setVisible(!isLive);
        btnBlue.setVisible(!isLive);
        btnGreen.setVisible(!isLive);
        btnYellow.setVisible(!isLive);
        btnWhite.setVisible(!isLive);
        btnRed.setVisible(!isLive);
        btnChord.setVisible(!isLive);

        if (isLive && liveViewer) {
            liveViewer->setContent(textEditor.getText());
            liveViewer->setMonospace(engine.getNoteIsMonospace());
            liveViewer->setFontSize(engine.getNoteFontSize());
        }

        resized();
    }

    ~GigNotepadComponent() override {
        saveCurrentTabContent();
        engine.removeNotesListener(this);
    }

    void loadFromEngine() {
        tabs = engine.getNotes();
        if (tabs.empty()) {
            tabs.push_back({ "Chords", "" });
            tabs.push_back({ "Lyrics", "" });
            tabs.push_back({ "Notes", "" });
            engine.setNotes(tabs);
        }
        activeTab = juce::jlimit(0, (int)tabs.size() - 1, engine.getActiveNoteTabIndex());
        fontTypeBtn.setButtonText(engine.getNoteIsMonospace() ? "MONO" : "SANS");

        rebuildTabButtons();
        loadActiveTabContent();
        applyEditorFont();
    }

    void saveCurrentTabContent() {
        if (activeTab >= 0 && activeTab < (int)tabs.size()) {
            tabs[activeTab].content = textEditor.getText();
            engine.setNotes(tabs);
            engine.setActiveNoteTabIndex(activeTab);
        }
    }

    void textEditorTextChanged(juce::TextEditor&) override {
        if (activeTab >= 0 && activeTab < (int)tabs.size()) {
            tabs[activeTab].content = textEditor.getText();
            engine.setNotes(tabs);
        }
    }

    void changeFontSize(float delta) {
        float sz = juce::jlimit(12.0f, 48.0f, engine.getNoteFontSize() + delta);
        engine.setNoteFontSize(sz);
        applyEditorFont();
    }

    void applyEditorFont() {
        float sz = engine.getNoteFontSize();
        if (sz < 12.0f) sz = 20.0f;
        fontSizeLabel.setText(juce::String((int)sz) + " pt", juce::dontSendNotification);

        juce::Font f = engine.getNoteIsMonospace()
            ? juce::Font(juce::FontOptions().withName(juce::Font::getDefaultMonospacedFontName()).withHeight(sz))
            : juce::Font(juce::FontOptions(sz));

        textEditor.setFont(f);
        if (liveViewer) {
            liveViewer->setFontSize(sz);
            liveViewer->setMonospace(engine.getNoteIsMonospace());
        }
    }

    void selectTab(int index) {
        if (index < 0 || index >= (int)tabs.size()) return;
        saveCurrentTabContent();
        activeTab = index;
        engine.setActiveNoteTabIndex(activeTab);
        rebuildTabButtons();
        loadActiveTabContent();
    }

    void loadActiveTabContent() {
        if (activeTab >= 0 && activeTab < (int)tabs.size()) {
            textEditor.setText(tabs[activeTab].content, false);
            if (liveViewer) liveViewer->setContent(tabs[activeTab].content);
        }
    }

    void rebuildTabButtons() {
        tabButtons.clear();
        for (int i = 0; i < (int)tabs.size(); ++i) {
            auto btn = std::make_unique<juce::TextButton>(tabs[i].title);
            btn->setClickingTogglesState(true);
            btn->setToggleState(i == activeTab, juce::dontSendNotification);
            btn->setRadioGroupId(1001);
            Fanfare::setButtonRole(*btn, (i == activeTab) ? Fanfare::ButtonRole::Primary : Fanfare::ButtonRole::Standard);

            int tabIdx = i;
            btn->onClick = [this, tabIdx] {
                selectTab(tabIdx);
            };
            addAndMakeVisible(btn.get());
            tabButtons.push_back(std::move(btn));
        }
        resized();
    }

    void addNewTabPrompt() {
        auto* alert = new juce::AlertWindow("New Tab", "Enter a title for this note tab:", juce::MessageBoxIconType::QuestionIcon);
        alert->addTextEditor("tabName", "Tab " + juce::String(tabs.size() + 1));
        alert->addButton("Add", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(true, juce::ModalCallbackFunction::create([this, alert](int result) {
            std::unique_ptr<juce::AlertWindow> windowDeleter(alert);
            if (result == 1) {
                juce::String name = alert->getTextEditorContents("tabName").trim();
                if (name.isNotEmpty()) {
                    saveCurrentTabContent();
                    tabs.push_back({ name, "" });
                    activeTab = (int)tabs.size() - 1;
                    engine.setNotes(tabs);
                    engine.setActiveNoteTabIndex(activeTab);
                    rebuildTabButtons();
                    loadActiveTabContent();
                }
            }
        }));
    }

    void showTabManagementMenu() {
        juce::PopupMenu menu;
        menu.addItem(1, "Rename Current Tab (" + (activeTab < (int)tabs.size() ? tabs[activeTab].title : "") + ")");
        if (tabs.size() > 1) {
            menu.addItem(2, "Delete Current Tab");
        }
        menu.addSeparator();
        menu.addItem(3, "Clear Tab Contents");

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&manageTabsBtn), [this](int result) {
            if (result == 1) {
                renameCurrentTabPrompt();
            } else if (result == 2) {
                deleteCurrentTab();
            } else if (result == 3) {
                textEditor.clear();
                saveCurrentTabContent();
                if (liveViewer) liveViewer->setContent("");
            }
        });
    }

    void renameCurrentTabPrompt() {
        if (activeTab < 0 || activeTab >= (int)tabs.size()) return;
        auto* alert = new juce::AlertWindow("Rename Tab", "Enter new tab title:", juce::MessageBoxIconType::QuestionIcon);
        alert->addTextEditor("tabName", tabs[activeTab].title);
        alert->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        alert->enterModalState(true, juce::ModalCallbackFunction::create([this, alert](int result) {
            std::unique_ptr<juce::AlertWindow> windowDeleter(alert);
            if (result == 1) {
                juce::String name = alert->getTextEditorContents("tabName").trim();
                if (name.isNotEmpty()) {
                    tabs[activeTab].title = name;
                    engine.setNotes(tabs);
                    rebuildTabButtons();
                }
            }
        }));
    }

    void deleteCurrentTab() {
        if (tabs.size() <= 1 || activeTab < 0 || activeTab >= (int)tabs.size()) return;
        tabs.erase(tabs.begin() + activeTab);
        activeTab = juce::jlimit(0, (int)tabs.size() - 1, activeTab);
        engine.setNotes(tabs);
        engine.setActiveNoteTabIndex(activeTab);
        rebuildTabButtons();
        loadActiveTabContent();
    }

    void importFilePrompt() {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Import Song Notes / Chords (.md, .txt, .html)",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
            "*.md;*.markdown;*.txt;*.html;*.htm"
        );

        auto folderChooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        fileChooser->launchAsync(folderChooserFlags, [this](const juce::FileChooser& chooser) {
            auto file = chooser.getResult();
            if (file.existsAsFile()) {
                juce::String rawText = file.loadFileAsString();
                juce::String ext = file.getFileExtension().toLowerCase();

                juce::String parsedText = rawText;
                if (ext == ".html" || ext == ".htm") {
                    parsedText = parseHtmlToStageFormat(rawText);
                }

                if (textEditor.getText().trim().isEmpty()) {
                    textEditor.setText(parsedText, false);
                    saveCurrentTabContent();
                    if (liveViewer) liveViewer->setContent(parsedText);
                } else {
                    auto* alert = new juce::AlertWindow("Import Note", "How would you like to import '" + file.getFileName() + "'?", juce::MessageBoxIconType::QuestionIcon);
                    alert->addButton("Replace Tab", 1);
                    alert->addButton("New Tab", 2);
                    alert->addButton("Cancel", 0);

                    alert->enterModalState(true, juce::ModalCallbackFunction::create([this, alert, parsedText, file](int res) {
                        std::unique_ptr<juce::AlertWindow> windowDeleter(alert);
                        if (res == 1) {
                            textEditor.setText(parsedText, false);
                            saveCurrentTabContent();
                            if (liveViewer) liveViewer->setContent(parsedText);
                        } else if (res == 2) {
                            saveCurrentTabContent();
                            juce::String tabName = file.getFileNameWithoutExtension();
                            tabs.push_back({ tabName, parsedText });
                            activeTab = (int)tabs.size() - 1;
                            engine.setNotes(tabs);
                            engine.setActiveNoteTabIndex(activeTab);
                            rebuildTabButtons();
                            loadActiveTabContent();
                        }
                    }));
                }
            }
        });
    }

    static juce::String parseHtmlToStageFormat(const juce::String& html) {
        juce::String text = html;

        while (true) {
            int s = text.indexOfIgnoreCase(0, "<script");
            if (s < 0) break;
            int e = text.indexOfIgnoreCase(s, "</script>");
            if (e < 0) break;
            text = text.substring(0, s) + text.substring(e + 9);
        }
        while (true) {
            int s = text.indexOfIgnoreCase(0, "<style");
            if (s < 0) break;
            int e = text.indexOfIgnoreCase(s, "</style>");
            if (e < 0) break;
            text = text.substring(0, s) + text.substring(e + 8);
        }

        for (int h = 1; h <= 6; ++h) {
            juce::String openTag = "<h" + juce::String(h);
            juce::String closeTag = "</h" + juce::String(h) + ">";
            juce::String hash = juce::String::repeatedString("#", h) + " ";

            while (true) {
                int s = text.indexOfIgnoreCase(0, openTag);
                if (s < 0) break;
                int closeTagStart = text.indexOf(s, ">");
                if (closeTagStart < 0) break;
                int e = text.indexOfIgnoreCase(closeTagStart, closeTag);
                if (e < 0) break;

                juce::String headerContent = text.substring(closeTagStart + 1, e);
                text = text.substring(0, s) + "\n\n" + hash + headerContent + "\n" + text.substring(e + closeTag.length());
            }
        }

        text = text.replace("<br>", "\n", true);
        text = text.replace("<br/>", "\n", true);
        text = text.replace("<br />", "\n", true);
        text = text.replace("</p>", "\n\n", true);
        text = text.replace("</div>", "\n", true);
        text = text.replace("</tr>", "\n", true);
        text = text.replace("<li>", "\n- ", true);
        text = text.replace("</li>", "", true);

        text = text.replace("<b>", "**", true);
        text = text.replace("</b>", "**", true);
        text = text.replace("<strong>", "**", true);
        text = text.replace("</strong>", "**", true);
        text = text.replace("<i>", "*", true);
        text = text.replace("</i>", "*", true);
        text = text.replace("<em>", "*", true);
        text = text.replace("</em>", "*", true);
        text = text.replace("<code>", "`", true);
        text = text.replace("</code>", "`", true);

        while (true) {
            int open = text.indexOf(0, "<");
            if (open < 0) break;
            int close = text.indexOf(open, ">");
            if (close < 0) break;
            text = text.substring(0, open) + text.substring(close + 1);
        }

        text = text.replace("&nbsp;", " ");
        text = text.replace("&amp;", "&");
        text = text.replace("&lt;", "<");
        text = text.replace("&gt;", ">");
        text = text.replace("&quot;", "\"");
        text = text.replace("&#39;", "'");
        text = text.replace("&apos;", "'");

        while (text.contains("\n\n\n")) {
            text = text.replace("\n\n\n", "\n\n");
        }

        return text.trim();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(ThemeManager::get(Theme::Role::panel));
        g.setColour(ThemeManager::get(Theme::Role::sunken));
        g.fillRect(0, 0, getWidth(), 40);

        g.setColour(ThemeManager::get(Theme::Role::border));
        g.drawRect(getLocalBounds(), 1);

        g.setColour(ThemeManager::get(Theme::Role::hairline));
        g.drawLine(0.0f, 40.0f, (float)getWidth(), 40.0f, 1.0f);
        g.drawLine(0.0f, 76.0f, (float)getWidth(), 76.0f, 1.0f);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(10);

        auto topRow = area.removeFromTop(26);
        songTitleLabel.setBounds(topRow.removeFromLeft(110));
        topRow.removeFromLeft(8);

        liveModeBtn.setBounds(topRow.removeFromLeft(110));
        topRow.removeFromLeft(8);

        zoomOutBtn.setBounds(topRow.removeFromLeft(28));
        fontSizeLabel.setBounds(topRow.removeFromLeft(46));
        zoomInBtn.setBounds(topRow.removeFromLeft(28));
        topRow.removeFromLeft(8);

        fontTypeBtn.setBounds(topRow.removeFromLeft(52));
        topRow.removeFromLeft(8);

        importBtn.setBounds(topRow.removeFromLeft(75));
        topRow.removeFromLeft(8);

        manageTabsBtn.setBounds(topRow.removeFromRight(65));
        topRow.removeFromRight(4);
        addTabBtn.setBounds(topRow.removeFromRight(55));
        topRow.removeFromRight(4);
        helpBtn.setBounds(topRow.removeFromRight(74));

        area.removeFromTop(8);

        auto tabRow = area.removeFromTop(26);

        bool isEditMode = !liveModeBtn.getToggleState();
        if (isEditMode) {
            auto fmtRow = tabRow.removeFromRight(378);
            btnChord.setBounds(fmtRow.removeFromRight(70).reduced(2, 0));
            btnRed.setBounds(fmtRow.removeFromRight(46).reduced(2, 0));
            btnWhite.setBounds(fmtRow.removeFromRight(60).reduced(2, 0));
            btnYellow.setBounds(fmtRow.removeFromRight(58).reduced(2, 0));
            btnGreen.setBounds(fmtRow.removeFromRight(56).reduced(2, 0));
            btnBlue.setBounds(fmtRow.removeFromRight(50).reduced(2, 0));
            btnBold.setBounds(fmtRow.removeFromRight(32).reduced(2, 0));
        }

        int tabCount = (int)tabButtons.size();
        if (tabCount > 0) {
            int tabW = juce::jmin(110, tabRow.getWidth() / tabCount);
            for (auto& btn : tabButtons) {
                btn->setBounds(tabRow.removeFromLeft(tabW).reduced(2, 0));
            }
        }

        area.removeFromTop(6);

        liveViewport.setBounds(area);
        if (liveViewer) liveViewer->setBounds(0, 0, area.getWidth() - 16, area.getHeight());
        textEditor.setBounds(area);
    }

private:
    FanfareEngine& engine;
    std::vector<Fanfare::NoteTab> tabs;
    int activeTab = 0;

    juce::Label songTitleLabel;
    juce::TextButton liveModeBtn;
    juce::TextButton zoomOutBtn;
    juce::TextButton zoomInBtn;
    juce::Label fontSizeLabel;
    juce::TextButton fontTypeBtn;
    juce::TextButton importBtn;
    juce::TextButton addTabBtn;
    juce::TextButton manageTabsBtn;
    juce::TextButton helpBtn{"HAAALP!"};

    juce::TextButton btnBold;
    juce::TextButton btnBlue;
    juce::TextButton btnGreen;
    juce::TextButton btnYellow;
    juce::TextButton btnWhite;
    juce::TextButton btnRed;
    juce::TextButton btnChord;

    juce::Viewport liveViewport;
    std::unique_ptr<StageLiveNotesViewer> liveViewer;
    juce::TextEditor textEditor;

    std::vector<std::unique_ptr<juce::TextButton>> tabButtons;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GigNotepadComponent)
};
