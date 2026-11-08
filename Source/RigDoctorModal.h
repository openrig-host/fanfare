#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "FanfareEngine.h"
#include "HelpLauncher.h"
#include "Logger.h"
#include "PluginBuildGate.h"
#include "RigLibrary.h"
#include "RigSerializer.h"
#include "SaveHistory.h"
#include "ThemeManager.h"
#include "VersionHistoryModal.h"

namespace Fanfare {

/**
 * RigDoctorModal
 * --------------
 * In-app diagnostic & surgical rehabilitation tool for Fanfare song setups (.json).
 *
 * Capabilities:
 *   1. Static health & schema inspection (channels, scenes, plugin chains, state blobs,
 *      external WAV/IR/MP3 assets, session-poisoned plugin paths).
 *   2. Automatic diff against sibling <song>.json.bak to spotlight recently added or
 *      edited plugins/patches that caused a previously healthy setup to stop loading.
 *   3. Log forensics scan of OpenRig_log.txt / OpenRig_log.bak.txt for stalls, timeouts,
 *      or state-restore faults involving this song or its plugins.
 *   4. Isolated per-plugin dry-run probe (STA message thread vs MTA worker thread, matching
 *      RigBuilder) with per-plugin timing and fault classification.
 *   5. Non-destructive in-place JSON rehabilitation (Keep / Disable / Reset Patch / Remove
 *      per plugin, Clean Slate toggle, Clear Session Locks, Restore .bak, Version History,
 *      Save Copy, Repair & Save, Repair & Load Now) preserving 100% of setup fields.
 */
class RigDoctorModal : public juce::Component,
                       public juce::TableListBoxModel {
public:
    struct Actions {
        std::function<void()> onClose;
        std::function<void(const juce::File&)> onLoadSong;
    };

    enum class RepairAction {
        Keep = 0,
        Disable,
        ResetPatch,
        Remove
    };

    enum class ProbeState {
        NotRun = 0,
        Testing,
        Passed,
        PatchFault,
        BuildFailed,
        TimedOut,
        Skipped
    };

    enum class ContainerKind {
        ChannelSlot = 0,
        AuxReturn,
        MasterFoh,
        MasterIem
    };

    struct PluginRow {
        ContainerKind container = ContainerKind::ChannelSlot;
        int ownerIndex = 0;   // channel index or aux index (0 for master)
        int chainIndex = 0;   // 0..2
        juce::String key;     // e.g. S:15:1, A:0:0, F:0, I:0
        juce::String locationLabel;
        juce::String slotName;
        juce::String pluginName;
        juce::String pluginPath;
        juce::var pluginVar;
        size_t decodedStateBytes = 0;
        bool base64Valid = true;
        bool enabledInRig = true;
        bool fileExists = false;
        bool requiresMessageThread = false;
        bool sessionPoisoned = false;
        juce::String diffTag; // e.g. "+NEW vs .bak", "REPLACED vs .bak", "PATCH EDITED vs .bak"

        RepairAction action = RepairAction::Keep;
        ProbeState probeState = ProbeState::NotRun;
        int probeMs = 0;
        juce::String probeDetail;
    };

    RigDoctorModal(FanfareEngine& eng, const juce::File& initialFile, Actions callbacks)
        : engine(eng), actions(std::move(callbacks))
    {
        setOpaque(false);

        styleHelpButton(helpBtn, "rig-doctor",
                        "Open the Fanfare Stage Manual to Chapter 23: Rig Doctor");
        addAndMakeVisible(helpBtn);

        closeBtn.setButtonText("CLOSE");
        setButtonRole(closeBtn, ButtonRole::Subtle);
        closeBtn.onClick = [this] {
            stopProbe();
            if (actions.onClose)
                actions.onClose();
        };
        addAndMakeVisible(closeBtn);

        browseBtn.setButtonText("BROWSE SONG...");
        setButtonRole(browseBtn, ButtonRole::Standard);
        browseBtn.onClick = [this] { browseForSongFile(); };
        addAndMakeVisible(browseBtn);

        rescanBtn.setButtonText("RESCAN");
        setButtonRole(rescanBtn, ButtonRole::Subtle);
        rescanBtn.onClick = [this] {
            if (currentFile.existsAsFile())
                inspectFile(currentFile);
        };
        addAndMakeVisible(rescanBtn);

        fileInfoLabel.setFont(BoutiqueLookAndFeel::uiFont(13.0f, true));
        fileInfoLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::text));
        addAndMakeVisible(fileInfoLabel);

        testAllBtn.setButtonText("TEST ALL PLUGINS");
        setButtonRole(testAllBtn, ButtonRole::Standard);
        testAllBtn.onClick = [this] { startProbe(false); };
        addAndMakeVisible(testAllBtn);

        testSelectedBtn.setButtonText("TEST SELECTED");
        setButtonRole(testSelectedBtn, ButtonRole::Subtle);
        testSelectedBtn.onClick = [this] { startProbe(true); };
        addAndMakeVisible(testSelectedBtn);

        stopTestBtn.setButtonText("STOP TEST");
        setButtonRole(stopTestBtn, ButtonRole::Danger);
        stopTestBtn.onClick = [this] { stopProbe(); };
        stopTestBtn.setEnabled(false);
        addAndMakeVisible(stopTestBtn);

        unloadLiveToggle.setButtonText("Unload live mixer before test");
        unloadLiveToggle.setTooltip("Recommended when testing single-instance VSTs (e.g. Roland Cloud) so live mixer instances cannot contend with the dry-run.");
        unloadLiveToggle.setToggleState(false, juce::dontSendNotification);
        addAndMakeVisible(unloadLiveToggle);

        clearLocksBtn.setButtonText("CLEAR SESSION LOCKS");
        setButtonRole(clearLocksBtn, ButtonRole::Subtle);
        clearLocksBtn.setTooltip("Clears the session skip-list for plugins that timed out earlier in this Fanfare session.");
        clearLocksBtn.onClick = [this] {
            clearPoisonedPaths();
            appendReportLine("[ACTION] Cleared session plugin lock list.");
            if (currentFile.existsAsFile())
                refreshPoisonStatus();
        };
        addAndMakeVisible(clearLocksBtn);

        table.setModel(this);
        table.setRowHeight(28);
        table.setColour(juce::ListBox::backgroundColourId, ThemeManager::get(Theme::Role::sunken));
        table.setColour(juce::ListBox::outlineColourId, ThemeManager::get(Theme::Role::border));
        table.setOutlineThickness(1);

        auto& hdr = table.getHeader();
        hdr.addColumn("Location", 1, 155, 110, 240);
        hdr.addColumn("Plugin", 2, 165, 110, 260);
        hdr.addColumn("Thread / State", 3, 125, 95, 175);
        hdr.addColumn("Health / Diff / Probe", 4, 215, 140, 340);
        hdr.addColumn("Surgical Action", 5, 220, 200, 250);
        addAndMakeVisible(table);

        reportBox.setMultiLine(true);
        reportBox.setReadOnly(true);
        reportBox.setScrollbarsShown(true);
        reportBox.setCaretVisible(false);
        reportBox.setFont(BoutiqueLookAndFeel::monoFont(11.5f, false));
        reportBox.setColour(juce::TextEditor::backgroundColourId, ThemeManager::get(Theme::Role::sunken));
        reportBox.setColour(juce::TextEditor::textColourId, ThemeManager::get(Theme::Role::text));
        reportBox.setColour(juce::TextEditor::outlineColourId, ThemeManager::get(Theme::Role::border));
        addAndMakeVisible(reportBox);

        freshBuildToggle.setButtonText("Clean Slate (freshBuild)");
        freshBuildToggle.setTooltip("When enabled, Fanfare unloads the previous song's plugins before building this song.");
        freshBuildToggle.onClick = [this] { updateFooterButtonStates(); };
        addAndMakeVisible(freshBuildToggle);

        clearMissingMediaToggle.setButtonText("Clear missing WAV/MP3 paths");
        clearMissingMediaToggle.setTooltip("Clears references to missing Sampler WAVs, IR WAVs, or MP3 files when saving.");
        clearMissingMediaToggle.onClick = [this] { updateFooterButtonStates(); };
        addAndMakeVisible(clearMissingMediaToggle);

        historyBtn.setButtonText("HISTORY...");
        setButtonRole(historyBtn, ButtonRole::Subtle);
        historyBtn.onClick = [this] { openVersionHistory(); };
        addAndMakeVisible(historyBtn);

        restoreBakBtn.setButtonText("RESTORE .BAK");
        setButtonRole(restoreBakBtn, ButtonRole::Subtle);
        restoreBakBtn.onClick = [this] { confirmRestoreBak(); };
        addAndMakeVisible(restoreBakBtn);

        resetEditsBtn.setButtonText("RESET EDITS");
        setButtonRole(resetEditsBtn, ButtonRole::Subtle);
        resetEditsBtn.onClick = [this] {
            for (auto& r : rows)
                r.action = RepairAction::Keep;
            freshBuildToggle.setToggleState(originalFreshBuild, juce::dontSendNotification);
            clearMissingMediaToggle.setToggleState(false, juce::dontSendNotification);
            table.updateContent();
            table.repaint();
            updateFooterButtonStates();
        };
        addAndMakeVisible(resetEditsBtn);

        saveCopyBtn.setButtonText("SAVE COPY...");
        setButtonRole(saveCopyBtn, ButtonRole::Standard);
        saveCopyBtn.onClick = [this] { saveRepairedCopyAs(); };
        addAndMakeVisible(saveCopyBtn);

        repairSaveBtn.setButtonText("REPAIR & SAVE");
        setButtonRole(repairSaveBtn, ButtonRole::Standard);
        repairSaveBtn.onClick = [this] { saveInPlace(false); };
        addAndMakeVisible(repairSaveBtn);

        repairLoadBtn.setButtonText("REPAIR & LOAD NOW");
        setButtonRole(repairLoadBtn, ButtonRole::Primary);
        repairLoadBtn.onClick = [this] { saveInPlace(true); };
        addAndMakeVisible(repairLoadBtn);

        setSize(920, 620);

        if (initialFile.existsAsFile()) {
            inspectFile(initialFile);
        } else {
            fileInfoLabel.setText("No song file selected - click BROWSE SONG... to inspect any .json setup.",
                                  juce::dontSendNotification);
            reportBox.setText("Select a .json song file (or right-click any song in the Songs sidebar -> Diagnose & repair...) to inspect its plugin chains, compare against .bak, check logs, run isolated plugin load tests, or surgically rehabilitate broken slots.");
            updateFooterButtonStates();
        }
    }

    ~RigDoctorModal() override {
        aliveToken->store(false, std::memory_order_release);
        stopProbe();
        table.setModel(nullptr);
    }

    void inspectFile(const juce::File& file) {
        stopProbe();
        currentFile = file;
        rows.clear();
        missingMediaSummary.clear();
        originalJsonText.clear();
        hasValidJson = false;

        if (!file.existsAsFile()) {
            fileInfoLabel.setText("File not found: " + file.getFullPathName(), juce::dontSendNotification);
            reportBox.setText("ERROR: Selected file does not exist on disk:\n" + file.getFullPathName());
            table.updateContent();
            updateFooterButtonStates();
            return;
        }

        originalJsonText = file.loadFileAsString();
        auto loaded = RigSerializer::loadFromString(originalJsonText);
        if (!loaded.ok) {
            fileInfoLabel.setText(file.getFileName() + "  |  INVALID JSON / SCHEMA", juce::dontSendNotification);
            juce::String errReport;
            errReport << "=== RIG DOCTOR REPORT: " << file.getFileName() << " ===\n";
            errReport << "CRITICAL: Setup JSON failed validation: " << loaded.errorMessage << "\n";
            juce::File bak = getBakFile();
            if (bak.existsAsFile()) {
                auto bakLoaded = RigSerializer::load(bak);
                errReport << "GOOD NEWS: Backup file '" << bak.getFileName() << "' ("
                          << formatBytes((size_t)bak.getSize()) << ", modified "
                          << bak.getLastModificationTime().toString(true, true, false, true)
                          << ") is " << (bakLoaded.ok ? "VALID! Click RESTORE .BAK below to recover." : "also unreadable.") << "\n";
            }
            reportBox.setText(errReport, false);
            table.updateContent();
            updateFooterButtonStates();
            return;
        }

        hasValidJson = true;
        const auto& rig = loaded.rig;
        originalFreshBuild = (bool)rig.getProperty("freshBuild", false);
        freshBuildToggle.setToggleState(originalFreshBuild, juce::dontSendNotification);
        clearMissingMediaToggle.setToggleState(false, juce::dontSendNotification);

        // Parse .bak if present so we can highlight exact changes!
        std::map<juce::String, PluginSnapshot> bakMap;
        juce::File bakFile = getBakFile();
        bool hasValidBak = false;
        if (bakFile.existsAsFile()) {
            auto bakLoaded = RigSerializer::load(bakFile);
            if (bakLoaded.ok) {
                hasValidBak = true;
                collectPluginSnapshots(bakLoaded.rig, bakMap);
            }
        }

        // Collect rows from current file
        int numChannels = 0;
        if (auto* chans = rig.getProperty("channels", juce::var()).getArray()) {
            numChannels = chans->size();
            for (int i = 0; i < chans->size(); ++i) {
                const auto& ch = chans->getReference(i);
                juce::String sName = ch.getProperty("name", "Slot " + juce::String(i + 1)).toString();
                if (auto* chain = ch.getProperty("chain", juce::var()).getArray()) {
                    for (int c = 0; c < chain->size(); ++c) {
                        const auto& pv = chain->getReference(c);
                        if (!pv.isObject())
                            continue;
                        juce::String loc = "Strip " + juce::String(i + 1) + " c" + juce::String(c + 1);
                        if (sName.isNotEmpty() && sName != ("Slot " + juce::String(i + 1)))
                            loc << " (" << sName << ")";
                        addPluginRow(ContainerKind::ChannelSlot, i, c,
                                     "S:" + juce::String(i) + ":" + juce::String(c),
                                     loc, sName, pv, hasValidBak ? &bakMap : nullptr);
                    }
                }

                // Check sampler & strip IR external files
                auto samplerObj = ch.getProperty("sampler", juce::var());
                if (samplerObj.isObject()) {
                    juce::String wavPath = samplerObj.getProperty("filePath", "").toString();
                    if (wavPath.isNotEmpty() && !juce::File(wavPath).existsAsFile())
                        missingMediaSummary.add("Strip " + juce::String(i + 1) + " Sampler WAV missing: " + wavPath);
                }
                auto stripObj = ch.getProperty("strip", juce::var());
                if (stripObj.isObject()) {
                    juce::String irPath = stripObj.getProperty("irPath", "").toString();
                    if (irPath.isNotEmpty() && !juce::File(irPath).existsAsFile())
                        missingMediaSummary.add("Strip " + juce::String(i + 1) + " Cabinet IR missing: " + irPath);
                }
            }
        }

        if (auto* auxes = rig.getProperty("auxReturns", juce::var()).getArray()) {
            for (int a = 0; a < auxes->size(); ++a) {
                const auto& ax = auxes->getReference(a);
                juce::String aName = "Aux " + juce::String(a + 1);
                if (auto* chain = ax.getProperty("chain", juce::var()).getArray()) {
                    for (int c = 0; c < chain->size(); ++c) {
                        const auto& pv = chain->getReference(c);
                        if (!pv.isObject())
                            continue;
                        addPluginRow(ContainerKind::AuxReturn, a, c,
                                     "A:" + juce::String(a) + ":" + juce::String(c),
                                     aName + " c" + juce::String(c + 1), aName, pv,
                                     hasValidBak ? &bakMap : nullptr);
                    }
                }
            }
        }

        if (auto* foh = rig.getProperty("fohChain", juce::var()).getArray()) {
            for (int c = 0; c < foh->size(); ++c) {
                const auto& pv = foh->getReference(c);
                if (!pv.isObject())
                    continue;
                addPluginRow(ContainerKind::MasterFoh, 0, c,
                             "F:" + juce::String(c),
                             "Master FOH c" + juce::String(c + 1), "Master FOH", pv,
                             hasValidBak ? &bakMap : nullptr);
            }
        }

        if (auto* iem = rig.getProperty("iemChain", juce::var()).getArray()) {
            for (int c = 0; c < iem->size(); ++c) {
                const auto& pv = iem->getReference(c);
                if (!pv.isObject())
                    continue;
                addPluginRow(ContainerKind::MasterIem, 0, c,
                             "I:" + juce::String(c),
                             "Master IEM c" + juce::String(c + 1), "Master IEM", pv,
                             hasValidBak ? &bakMap : nullptr);
            }
        }

        int numScenes = 0;
        if (auto* scenes = rig.getProperty("scenes", juce::var()).getArray()) {
            numScenes = scenes->size();
            for (int s = 0; s < scenes->size(); ++s) {
                const auto& sc = scenes->getReference(s);
                juce::String scName = sc.getProperty("name", "Scene " + juce::String(s + 1)).toString();
                if (auto* states = sc.getProperty("states", juce::var()).getArray()) {
                    for (int i = 0; i < states->size(); ++i) {
                        juce::String irPath = states->getReference(i).getProperty("irPath", "").toString();
                        if (irPath.isNotEmpty() && !juce::File(irPath).existsAsFile())
                            missingMediaSummary.add("Scene '" + scName + "' Strip " + juce::String(i + 1) + " IR missing: " + irPath);
                    }
                }
            }
        }

        auto mp3Obj = rig.getProperty("mp3Player", juce::var());
        if (mp3Obj.isObject()) {
            if (auto* playlist = mp3Obj.getProperty("playlist", juce::var()).getArray()) {
                for (int k = 0; k < playlist->size(); ++k) {
                    juce::String p = playlist->getReference(k).getProperty("path", "").toString();
                    if (p.isNotEmpty() && !juce::File(p).existsAsFile())
                        missingMediaSummary.add("MP3 Playlist track missing: " + p);
                }
            }
        }

        missingMediaSummary.removeDuplicates(false);

        // Header summary
        juce::String hdrSummary;
        hdrSummary << file.getFileName()
                   << "   |   " << formatBytes((size_t)file.getSize())
                   << "   |   " << numChannels << " strips, "
                   << (int)rows.size() << " plugins, "
                   << numScenes << " scenes";
        fileInfoLabel.setText(hdrSummary, juce::dontSendNotification);

        // Build full diagnostic report text
        juce::String report;
        report << "=== RIG DOCTOR DIAGNOSTIC REPORT: " << file.getFileNameWithoutExtension() << " ===\n";
        report << "Path: " << file.getFullPathName()
               << "  (Modified: " << file.getLastModificationTime().toString(true, true, false, true)
               << ", Schema v" << loaded.detectedVersion
               << ", Clean Slate: " << (originalFreshBuild ? "ON" : "OFF") << ")\n";

        int missingVstCount = 0;
        int poisonedCount = 0;
        int badBase64Count = 0;
        for (const auto& r : rows) {
            if (!r.fileExists) ++missingVstCount;
            if (r.sessionPoisoned) ++poisonedCount;
            if (!r.base64Valid) ++badBase64Count;
        }

        if (missingVstCount == 0 && poisonedCount == 0 && badBase64Count == 0 && missingMediaSummary.isEmpty()) {
            report << "[STATIC CHECK] JSON structure, " << (int)rows.size()
                   << " VST3 paths, Base64 state blobs, and media references are all valid on disk.\n";
        } else {
            if (missingVstCount > 0)
                report << "[WARN] " << missingVstCount << " plugin VST3 file(s) are NOT installed at their saved paths.\n";
            if (poisonedCount > 0)
                report << "[WARN] " << poisonedCount << " plugin(s) are currently SESSION-LOCKED from an earlier timeout (click CLEAR SESSION LOCKS to retry).\n";
            if (badBase64Count > 0)
                report << "[WARN] " << badBase64Count << " plugin state blob(s) failed Base64 decoding (use RESET PATCH on affected rows).\n";
            for (const auto& m : missingMediaSummary)
                report << "[MEDIA] " << m << "\n";
        }

        // Diff vs .bak
        if (hasValidBak) {
            juce::StringArray diffLines;
            std::map<juce::String, bool> seenKeys;
            for (const auto& r : rows) {
                seenKeys[r.key] = true;
                if (r.diffTag.isNotEmpty()) {
                    diffLines.add("  " + r.diffTag + " -> " + r.locationLabel + ": " + r.pluginName +
                                  " (" + formatBytes(r.decodedStateBytes) + " patch)");
                }
            }
            for (const auto& kv : bakMap) {
                if (!seenKeys[kv.first]) {
                    diffLines.add("  -REMOVED vs .bak -> " + kv.second.location + ": " + kv.second.name);
                }
            }
            report << "[DIFF vs .bak (" << bakFile.getLastModificationTime().toString(true, true, false, true) << ")] ";
            if (diffLines.isEmpty()) {
                report << "Plugin chains & patch blobs match .bak identically.\n";
            } else {
                report << diffLines.size() << " plugin/patch difference(s) found:\n"
                       << diffLines.joinIntoString("\n") << "\n";
            }
        } else {
            report << "[DIFF vs .bak] No sibling .bak file found for comparison.\n";
        }

        // Recent log forensics
        juce::StringArray logClues = scanLogsForClues(file);
        if (!logClues.isEmpty()) {
            report << "[RECENT LOG CLUES (OpenRig_log)]\n";
            for (const auto& l : logClues)
                report << "  " << l << "\n";
        }

        report << "[TIP] Click 'TEST ALL PLUGINS' above to dry-run every plugin in isolation and measure exact load times, or set any suspect plugin's Surgical Action to RESET PATCH / DISABLE / REMOVE.";

        reportBox.setText(report, false);
        table.updateContent();
        if (!rows.empty())
            table.selectRow(0);
        updateFooterButtonStates();
        repaint();
    }

    int getNumRows() override { return (int)rows.size(); }

    void paintRowBackground(juce::Graphics& g, int rowNumber, int, int, bool rowIsSelected) override {
        const auto& t = ThemeManager::getInstance().active();
        if (rowIsSelected) {
            g.fillAll(t.accent.withAlpha(t.dark ? 0.22f : 0.16f));
            return;
        }
        if (rowNumber >= 0 && rowNumber < (int)rows.size()) {
            const auto& r = rows[(size_t)rowNumber];
            if (r.action == RepairAction::Remove) {
                g.fillAll(t.danger.withAlpha(0.10f));
                return;
            }
            if (r.action == RepairAction::ResetPatch || r.action == RepairAction::Disable) {
                g.fillAll(t.warn.withAlpha(0.10f));
                return;
            }
        }
        if (rowNumber % 2 == 1)
            g.fillAll(t.text.withAlpha(0.025f));
    }

    void paintCell(juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool) override {
        if (rowNumber < 0 || rowNumber >= (int)rows.size())
            return;

        const auto& t = ThemeManager::getInstance().active();
        const auto& r = rows[(size_t)rowNumber];
        const bool strike = (r.action == RepairAction::Remove);

        if (columnId == 1) {
            g.setColour(strike ? t.textFaint : t.text);
            g.setFont(BoutiqueLookAndFeel::uiFont(12.0f, false));
            g.drawText(r.locationLabel, 8, 0, width - 12, height, juce::Justification::centredLeft, true);
        } else if (columnId == 2) {
            g.setColour(strike ? t.textFaint : (r.fileExists ? t.text : t.danger));
            g.setFont(BoutiqueLookAndFeel::uiFont(12.5f, true));
            juce::String title = r.pluginName;
            if (!r.enabledInRig)
                title << " (off)";
            g.drawText(title, 8, 0, width - 12, height, juce::Justification::centredLeft, true);
        } else if (columnId == 3) {
            g.setColour(t.textDim);
            g.setFont(BoutiqueLookAndFeel::monoFont(11.0f, false));
            juce::String threadStr = r.requiresMessageThread ? "STA Msg" : "MTA Work";
            threadStr << " | " << formatBytes(r.decodedStateBytes);
            g.drawText(threadStr, 6, 0, width - 10, height, juce::Justification::centredLeft, true);
        } else if (columnId == 4) {
            juce::String statusText;
            juce::Colour statusCol = t.textDim;

            if (r.probeState != ProbeState::NotRun) {
                switch (r.probeState) {
                    case ProbeState::Testing:
                        statusText = "TESTING...";
                        statusCol = t.accent;
                        break;
                    case ProbeState::Passed:
                        statusText = "PASS (" + juce::String(r.probeMs) + " ms)";
                        statusCol = (r.probeMs > 5000) ? t.warn : t.ok;
                        break;
                    case ProbeState::PatchFault:
                        statusText = "PATCH FAULT -> Reset Patch";
                        statusCol = t.danger;
                        break;
                    case ProbeState::BuildFailed:
                        statusText = "FAILED: " + r.probeDetail;
                        statusCol = t.danger;
                        break;
                    case ProbeState::TimedOut:
                        statusText = "STALLED (" + juce::String(r.probeMs / 1000) + "s) -> Remove/Reset";
                        statusCol = t.danger;
                        break;
                    case ProbeState::Skipped:
                        statusText = "SKIPPED (" + r.probeDetail + ")";
                        statusCol = t.textFaint;
                        break;
                    default:
                        break;
                }
            } else if (!r.fileExists) {
                statusText = "MISSING VST3 ON DISK";
                statusCol = t.danger;
            } else if (r.sessionPoisoned) {
                statusText = "SESSION LOCKED (stalled)";
                statusCol = t.danger;
            } else if (!r.base64Valid) {
                statusText = "CORRUPT BASE64 STATE";
                statusCol = t.danger;
            } else if (r.diffTag.isNotEmpty()) {
                statusText = r.diffTag;
                statusCol = t.warn;
            } else {
                statusText = "Installed OK";
                statusCol = t.ok;
            }

            g.setColour(statusCol);
            g.setFont(BoutiqueLookAndFeel::monoFont(11.0f, r.probeState == ProbeState::TimedOut || !r.fileExists));
            g.drawText(statusText, 6, 0, width - 10, height, juce::Justification::centredLeft, true);
        }
    }

    juce::Component* refreshComponentForCell(int rowNumber, int columnId, bool,
                                             juce::Component* existingComponentToUpdate) override {
        if (columnId != 5) {
            delete existingComponentToUpdate;
            return nullptr;
        }
        if (rowNumber < 0 || rowNumber >= (int)rows.size()) {
            delete existingComponentToUpdate;
            return nullptr;
        }

        auto* cell = dynamic_cast<ActionCell*>(existingComponentToUpdate);
        if (cell == nullptr) {
            delete existingComponentToUpdate;
            cell = new ActionCell(*this);
        }
        cell->setRow(rowNumber, rows[(size_t)rowNumber].action);
        return cell;
    }

    void setRowAction(int rowIndex, RepairAction act) {
        if (rowIndex < 0 || rowIndex >= (int)rows.size())
            return;
        rows[(size_t)rowIndex].action = act;
        table.repaintRow(rowIndex);
        updateFooterButtonStates();
    }

    void paint(juce::Graphics& g) override {
        ModalFrame::paintOverlayScrimAndPanel(
            g, getLocalBounds(), getLocalBounds(),
            "RIG DOCTOR - SETUP DIAGNOSTICS & SURGICAL REPAIR", 38);
    }

    void resized() override {
        auto b = getLocalBounds().reduced(14);
        auto topHeader = b.removeFromTop(26);
        closeBtn.setBounds(topHeader.removeFromRight(76).withHeight(24));
        topHeader.removeFromRight(6);
        helpBtn.setBounds(topHeader.removeFromRight(58).withHeight(24));
        b.removeFromTop(10);

        // File bar
        auto fileRow = b.removeFromTop(28);
        rescanBtn.setBounds(fileRow.removeFromRight(80));
        fileRow.removeFromRight(6);
        browseBtn.setBounds(fileRow.removeFromRight(126));
        fileRow.removeFromRight(10);
        fileInfoLabel.setBounds(fileRow);

        b.removeFromTop(8);

        // Probe bar
        auto probeRow = b.removeFromTop(26);
        testAllBtn.setBounds(probeRow.removeFromLeft(142));
        probeRow.removeFromLeft(6);
        testSelectedBtn.setBounds(probeRow.removeFromLeft(118));
        probeRow.removeFromLeft(6);
        stopTestBtn.setBounds(probeRow.removeFromLeft(94));
        probeRow.removeFromLeft(12);
        clearLocksBtn.setBounds(probeRow.removeFromRight(158));
        probeRow.removeFromRight(8);
        unloadLiveToggle.setBounds(probeRow);

        b.removeFromTop(8);

        // Footer (bottom)
        auto footerRow = b.removeFromBottom(30);
        b.removeFromBottom(8);

        // Options row above footer
        auto optRow = b.removeFromBottom(24);
        b.removeFromBottom(8);

        freshBuildToggle.setBounds(optRow.removeFromLeft(195));
        optRow.removeFromLeft(10);
        clearMissingMediaToggle.setBounds(optRow.removeFromLeft(230));

        historyBtn.setBounds(footerRow.removeFromLeft(98));
        footerRow.removeFromLeft(6);
        restoreBakBtn.setBounds(footerRow.removeFromLeft(112));
        footerRow.removeFromLeft(6);
        resetEditsBtn.setBounds(footerRow.removeFromLeft(98));

        repairLoadBtn.setBounds(footerRow.removeFromRight(162));
        footerRow.removeFromRight(6);
        repairSaveBtn.setBounds(footerRow.removeFromRight(126));
        footerRow.removeFromRight(6);
        saveCopyBtn.setBounds(footerRow.removeFromRight(112));

        // Split remaining center area between plugin table and forensic report well
        const int reportH = juce::jlimit(110, 175, b.getHeight() / 3);
        reportBox.setBounds(b.removeFromBottom(reportH));
        b.removeFromBottom(8);
        table.setBounds(b);
    }

private:
    class ActionCell : public juce::Component {
    public:
        explicit ActionCell(RigDoctorModal& ownerModal) : owner(ownerModal) {
            setupBtn(keepBtn, "KEEP", RepairAction::Keep);
            setupBtn(disableBtn, "OFF", RepairAction::Disable);
            setupBtn(resetBtn, "RESET", RepairAction::ResetPatch);
            setupBtn(removeBtn, "REMOVE", RepairAction::Remove);
            keepBtn.setTooltip("Keep plugin and its saved patch unchanged.");
            disableBtn.setTooltip("Keep plugin in slot, but set enabled = false.");
            resetBtn.setTooltip("Keep plugin in slot, but clear its saved patch blob to default.");
            removeBtn.setTooltip("Remove this plugin from the slot (preserves slot routing, faders, scenes & CC maps).");
        }

        void setRow(int r, RepairAction current) {
            rowIdx = r;
            activeAction = current;
            styleButtons();
        }

        void resized() override {
            auto area = getLocalBounds().reduced(3, 3);
            const int gap = 3;
            const int w = (area.getWidth() - gap * 3) / 4;
            keepBtn.setBounds(area.removeFromLeft(w));
            area.removeFromLeft(gap);
            disableBtn.setBounds(area.removeFromLeft(w));
            area.removeFromLeft(gap);
            resetBtn.setBounds(area.removeFromLeft(w));
            area.removeFromLeft(gap);
            removeBtn.setBounds(area);
        }

    private:
        void setupBtn(juce::TextButton& b, const juce::String& text, RepairAction act) {
            b.setButtonText(text);
            setButtonRole(b, ButtonRole::Subtle);
            b.onClick = [this, act] { owner.setRowAction(rowIdx, act); };
            addAndMakeVisible(b);
        }

        void styleButtons() {
            setButtonRole(keepBtn, activeAction == RepairAction::Keep ? ButtonRole::Primary : ButtonRole::Subtle);
            setButtonRole(disableBtn, activeAction == RepairAction::Disable ? ButtonRole::Standard : ButtonRole::Subtle);
            setButtonRole(resetBtn, activeAction == RepairAction::ResetPatch ? ButtonRole::Standard : ButtonRole::Subtle);
            setButtonRole(removeBtn, activeAction == RepairAction::Remove ? ButtonRole::Danger : ButtonRole::Subtle);
        }

        RigDoctorModal& owner;
        int rowIdx = -1;
        RepairAction activeAction = RepairAction::Keep;
        juce::TextButton keepBtn, disableBtn, resetBtn, removeBtn;
    };

    struct PluginSnapshot {
        juce::String location;
        juce::String name;
        juce::String path;
        juce::String state;
    };

    static void collectPluginSnapshots(const juce::var& rig, std::map<juce::String, PluginSnapshot>& out) {
        auto addFromChain = [&](const juce::String& keyPrefix, const juce::String& locPrefix, const juce::var& chainVar) {
            if (auto* chain = chainVar.getArray()) {
                for (int c = 0; c < chain->size(); ++c) {
                    const auto& pv = chain->getReference(c);
                    if (!pv.isObject())
                        continue;
                    PluginSnapshot snap;
                    snap.location = locPrefix + " c" + juce::String(c + 1);
                    snap.path = pv.getProperty("path", "").toString();
                    snap.name = pv.getProperty("name", "").toString();
                    if (snap.name.isEmpty() && snap.path.isNotEmpty())
                        snap.name = juce::File(snap.path).getFileNameWithoutExtension();
                    snap.state = pv.getProperty("state", "").toString();
                    out[keyPrefix + juce::String(c)] = std::move(snap);
                }
            }
        };

        if (auto* chans = rig.getProperty("channels", juce::var()).getArray()) {
            for (int i = 0; i < chans->size(); ++i) {
                const auto& ch = chans->getReference(i);
                addFromChain("S:" + juce::String(i) + ":", "Strip " + juce::String(i + 1),
                             ch.getProperty("chain", juce::var()));
            }
        }
        if (auto* auxes = rig.getProperty("auxReturns", juce::var()).getArray()) {
            for (int a = 0; a < auxes->size(); ++a) {
                addFromChain("A:" + juce::String(a) + ":", "Aux " + juce::String(a + 1),
                             auxes->getReference(a).getProperty("chain", juce::var()));
            }
        }
        addFromChain("F:", "Master FOH", rig.getProperty("fohChain", juce::var()));
        addFromChain("I:", "Master IEM", rig.getProperty("iemChain", juce::var()));
    }

    void addPluginRow(ContainerKind kind, int ownerIdx, int chainIdx,
                      const juce::String& key, const juce::String& locLabel,
                      const juce::String& slotName, const juce::var& pv,
                      const std::map<juce::String, PluginSnapshot>* bakMap) {
        PluginRow row;
        row.container = kind;
        row.ownerIndex = ownerIdx;
        row.chainIndex = chainIdx;
        row.key = key;
        row.locationLabel = locLabel;
        row.slotName = slotName;
        row.pluginVar = pv;
        row.pluginPath = pv.getProperty("path", "").toString();
        row.pluginName = pv.getProperty("name", "").toString();
        if (row.pluginName.isEmpty() && row.pluginPath.isNotEmpty())
            row.pluginName = juce::File(row.pluginPath).getFileNameWithoutExtension();
        if (row.pluginName.isEmpty())
            row.pluginName = "Unknown Plugin";

        row.enabledInRig = (bool)pv.getProperty("enabled", true);
        row.fileExists = row.pluginPath.isNotEmpty() && juce::File(row.pluginPath).existsAsFile();
        row.requiresMessageThread = pluginRequiresMessageThread(row.pluginPath);
        row.sessionPoisoned = isPathPoisoned(row.pluginPath);

        juce::String stateB64 = pv.getProperty("state", "").toString();
        if (stateB64.isNotEmpty()) {
            juce::MemoryBlock mb;
            row.base64Valid = mb.fromBase64Encoding(stateB64);
            row.decodedStateBytes = mb.getSize();
        }

        if (bakMap != nullptr) {
            auto it = bakMap->find(key);
            if (it == bakMap->end()) {
                row.diffTag = "+NEW vs .bak";
            } else if (!it->second.path.equalsIgnoreCase(row.pluginPath)) {
                row.diffTag = "REPLACED (" + it->second.name + ")";
            } else if (it->second.state != stateB64) {
                row.diffTag = "PATCH EDITED vs .bak";
            }
        }

        rows.push_back(std::move(row));
    }

    static juce::String formatBytes(size_t bytes) {
        if (bytes >= 1024 * 1024)
            return juce::String((double)bytes / (1024.0 * 1024.0), 2) + " MB";
        if (bytes >= 1024)
            return juce::String((double)bytes / 1024.0, 1) + " KB";
        return juce::String((int)bytes) + " B";
    }

    juce::File getBakFile() const {
        if (currentFile == juce::File{})
            return {};
        return currentFile.getSiblingFile(currentFile.getFileName() + ".bak");
    }

    static juce::String readLogTail(const juce::File& f, juce::int64 maxBytes = 96 * 1024) {
        if (!f.existsAsFile())
            return {};
        juce::FileInputStream in(f);
        if (!in.openedOk())
            return {};
        const juce::int64 len = in.getTotalLength();
        if (len <= 0)
            return {};
        const juce::int64 startOffset = juce::jmax<juce::int64>(0, len - maxBytes);
        in.setPosition(startOffset);
        juce::MemoryBlock mb;
        in.readIntoMemoryBlock(mb);
        return mb.toString();
    }

    juce::StringArray scanLogsForClues(const juce::File& songFile) const {
        juce::StringArray clues;
        const juce::String songStem = songFile.getFileNameWithoutExtension();

        juce::String combined = readLogTail(FanfareLog::getLogFile().getSiblingFile("OpenRig_log.bak.txt"), 64 * 1024)
                              + "\n"
                              + readLogTail(FanfareLog::getLogFile(), 96 * 1024);

        juce::StringArray lines;
        lines.addLines(combined);

        for (int i = juce::jmax(0, lines.size() - 800); i < lines.size(); ++i) {
            const auto& ln = lines[i];
            const bool mentionsSong = songStem.isNotEmpty() && ln.containsIgnoreCase(songStem);
            const bool isProblem = ln.containsIgnoreCase("stalled") ||
                                   ln.containsIgnoreCase("timed out") ||
                                   ln.containsIgnoreCase("poison") ||
                                   ln.containsIgnoreCase("faulted") ||
                                   ln.containsIgnoreCase("Failed to load rig");
            if (!mentionsSong && !isProblem)
                continue;

            // Keep warnings relevant to plugins present in this setup or the song itself
            bool matchesSetupPlugin = mentionsSong;
            if (!matchesSetupPlugin) {
                for (const auto& r : rows) {
                    if (r.pluginName.isNotEmpty() && ln.containsIgnoreCase(r.pluginName)) {
                        matchesSetupPlugin = true;
                        break;
                    }
                }
            }
            if (matchesSetupPlugin)
                clues.add(ln.trim());
        }

        while (clues.size() > 8)
            clues.remove(0);
        return clues;
    }

    void refreshPoisonStatus() {
        for (auto& r : rows)
            r.sessionPoisoned = isPathPoisoned(r.pluginPath);
        clearLocksBtn.setEnabled(!getPoisonedPaths().empty());
        table.repaint();
    }

    bool hasPendingEdits() const {
        if (!hasValidJson)
            return false;
        if (freshBuildToggle.getToggleState() != originalFreshBuild)
            return true;
        if (clearMissingMediaToggle.getToggleState() && !missingMediaSummary.isEmpty())
            return true;
        for (const auto& r : rows) {
            if (r.action != RepairAction::Keep)
                return true;
        }
        return false;
    }

    void updateFooterButtonStates() {
        const bool ok = hasValidJson && currentFile.existsAsFile();
        const bool probing = probeRunning.load(std::memory_order_acquire);
        const bool dirty = hasPendingEdits();

        testAllBtn.setEnabled(ok && !rows.empty() && !probing);
        testSelectedBtn.setEnabled(ok && !rows.empty() && !probing);
        stopTestBtn.setEnabled(probing);
        browseBtn.setEnabled(!probing);
        rescanBtn.setEnabled(currentFile.existsAsFile() && !probing);

        clearLocksBtn.setEnabled(!getPoisonedPaths().empty());
        clearMissingMediaToggle.setEnabled(ok && !missingMediaSummary.isEmpty());
        freshBuildToggle.setEnabled(ok);

        historyBtn.setEnabled(currentFile != juce::File{} && !probing);
        restoreBakBtn.setEnabled(getBakFile().existsAsFile() && !probing);
        resetEditsBtn.setEnabled(dirty && !probing);
        saveCopyBtn.setEnabled(ok && !probing);
        repairSaveBtn.setEnabled(ok && dirty && !probing);
        repairLoadBtn.setEnabled(ok && !probing);
        repairLoadBtn.setButtonText(dirty ? "REPAIR & LOAD NOW" : "LOAD SONG NOW");
    }

    void appendReportLine(const juce::String& line) {
        reportBox.moveCaretToEnd();
        reportBox.insertTextAtCaret("\n" + line);
        reportBox.moveCaretToEnd();
    }

    // Build a rehabilitated copy of the parsed JSON tree without dropping any fields.
    bool buildRepairedRig(juce::var& outRig, juce::String& outError) const {
        auto loaded = RigSerializer::loadFromString(originalJsonText);
        if (!loaded.ok) {
            outError = loaded.errorMessage;
            return false;
        }
        outRig = loaded.rig;
        auto* root = outRig.getDynamicObject();
        if (root == nullptr) {
            outError = "Root JSON is not an object.";
            return false;
        }

        root->setProperty("freshBuild", freshBuildToggle.getToggleState());

        for (const auto& r : rows) {
            if (r.action == RepairAction::Keep)
                continue;

            juce::Array<juce::var>* targetChain = nullptr;
            if (r.container == ContainerKind::ChannelSlot) {
                if (auto* chans = root->getProperty("channels").getArray()) {
                    if (r.ownerIndex >= 0 && r.ownerIndex < chans->size()) {
                        if (auto* chObj = chans->getReference(r.ownerIndex).getDynamicObject())
                            targetChain = chObj->getProperty("chain").getArray();
                    }
                }
            } else if (r.container == ContainerKind::AuxReturn) {
                if (auto* auxes = root->getProperty("auxReturns").getArray()) {
                    if (r.ownerIndex >= 0 && r.ownerIndex < auxes->size()) {
                        if (auto* axObj = auxes->getReference(r.ownerIndex).getDynamicObject())
                            targetChain = axObj->getProperty("chain").getArray();
                    }
                }
            } else if (r.container == ContainerKind::MasterFoh) {
                targetChain = root->getProperty("fohChain").getArray();
            } else if (r.container == ContainerKind::MasterIem) {
                targetChain = root->getProperty("iemChain").getArray();
            }

            if (targetChain == nullptr || r.chainIndex < 0 || r.chainIndex >= targetChain->size())
                continue;

            if (r.action == RepairAction::Remove) {
                targetChain->set(r.chainIndex, juce::var());
            } else if (auto* pObj = targetChain->getReference(r.chainIndex).getDynamicObject()) {
                if (r.action == RepairAction::Disable) {
                    pObj->setProperty("enabled", false);
                } else if (r.action == RepairAction::ResetPatch) {
                    pObj->setProperty("state", "");
                }
            }
        }

        if (clearMissingMediaToggle.getToggleState()) {
            if (auto* chans = root->getProperty("channels").getArray()) {
                for (int i = 0; i < chans->size(); ++i) {
                    if (auto* chObj = chans->getReference(i).getDynamicObject()) {
                        if (auto* samp = chObj->getProperty("sampler").getDynamicObject()) {
                            juce::String p = samp->getProperty("filePath").toString();
                            if (p.isNotEmpty() && !juce::File(p).existsAsFile()) {
                                samp->setProperty("filePath", "");
                                samp->setProperty("enabled", false);
                            }
                        }
                        if (auto* strip = chObj->getProperty("strip").getDynamicObject()) {
                            juce::String ir = strip->getProperty("irPath").toString();
                            if (ir.isNotEmpty() && !juce::File(ir).existsAsFile()) {
                                strip->setProperty("irPath", "");
                                strip->setProperty("irEnabled", false);
                            }
                        }
                    }
                }
            }
            if (auto* scenes = root->getProperty("scenes").getArray()) {
                for (int s = 0; s < scenes->size(); ++s) {
                    if (auto* scObj = scenes->getReference(s).getDynamicObject()) {
                        if (auto* states = scObj->getProperty("states").getArray()) {
                            for (int i = 0; i < states->size(); ++i) {
                                if (auto* stObj = states->getReference(i).getDynamicObject()) {
                                    juce::String ir = stObj->getProperty("irPath").toString();
                                    if (ir.isNotEmpty() && !juce::File(ir).existsAsFile()) {
                                        stObj->setProperty("irPath", "");
                                        stObj->setProperty("irEnabled", false);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (auto* mp3Obj = root->getProperty("mp3Player").getDynamicObject()) {
                if (auto* playlist = mp3Obj->getProperty("playlist").getArray()) {
                    for (int k = playlist->size() - 1; k >= 0; --k) {
                        juce::String p = playlist->getReference(k).getProperty("path", "").toString();
                        if (p.isNotEmpty() && !juce::File(p).existsAsFile())
                            playlist->remove(k);
                    }
                }
            }
        }

        outError = RigSerializer::validate(outRig);
        return outError.isEmpty();
    }

    void browseForSongFile() {
        auto startDir = currentFile.existsAsFile()
                            ? currentFile.getParentDirectory()
                            : RigLibrary::getSongsDirectory();
        fileChooser = std::make_unique<juce::FileChooser>("Select Song Setup (.json) to Diagnose",
                                                          startDir, "*.json");
        juce::Component::SafePointer<RigDoctorModal> safeThis(this);
        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis](const juce::FileChooser& fc) {
                if (safeThis == nullptr)
                    return;
                auto f = fc.getResult();
                if (f.existsAsFile())
                    safeThis->inspectFile(f);
            });
    }

    void openVersionHistory() {
        if (currentFile == juce::File{})
            return;
        juce::Component::SafePointer<RigDoctorModal> safeThis(this);
        showVersionHistoryDialog(currentFile, [safeThis](const juce::File& restored) {
            if (safeThis != nullptr) {
                safeThis->inspectFile(restored);
                safeThis->appendReportLine("[RESTORED] Restored snapshot from Version History.");
            }
        });
    }

    void confirmRestoreBak() {
        juce::File bak = getBakFile();
        if (!bak.existsAsFile() || currentFile == juce::File{})
            return;

        juce::Component::SafePointer<RigDoctorModal> safeThis(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::QuestionIcon)
                .withTitle("Restore .bak backup")
                .withMessage("Restore \"" + currentFile.getFileName() + "\" from \"" +
                             bak.getFileName() + "\" (modified " +
                             bak.getLastModificationTime().toString(true, true, false, true) +
                             ")?\n\nYour current file on disk will be saved into Version History first so nothing is lost.")
                .withButton("Restore .bak")
                .withButton("Cancel"),
            [safeThis, bak](int res) {
                if (res != 1 || safeThis == nullptr)
                    return;
                juce::String err;
                if (!SaveHistory::restoreVersion(bak, safeThis->currentFile, err)) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon, "Restore Failed", err);
                    return;
                }
                safeThis->inspectFile(safeThis->currentFile);
                safeThis->appendReportLine("[RESTORED] Restored setup from .bak backup (previous state preserved in Version History).");
            });
    }

    void saveRepairedCopyAs() {
        juce::var repaired;
        juce::String err;
        if (!buildRepairedRig(repaired, err)) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "Cannot Build Repaired Setup", err);
            return;
        }

        juce::File defaultTarget = currentFile.getSiblingFile(
            currentFile.getFileNameWithoutExtension() + " (Repaired).json");
        fileChooser = std::make_unique<juce::FileChooser>("Save Repaired Song Copy As",
                                                          defaultTarget, "*.json");
        juce::String jsonOut = juce::JSON::toString(repaired);
        juce::Component::SafePointer<RigDoctorModal> safeThis(this);
        fileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode |
                juce::FileBrowserComponent::canSelectFiles |
                juce::FileBrowserComponent::warnAboutOverwriting,
            [safeThis, jsonOut](const juce::FileChooser& fc) {
                if (safeThis == nullptr)
                    return;
                auto dest = fc.getResult();
                if (dest == juce::File{})
                    return;
                if (!dest.hasFileExtension(".json"))
                    dest = dest.withFileExtension(".json");
                SaveHistory::recordSave(dest);
                if (!RigSerializer::save(dest, jsonOut)) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon, "Save Failed",
                        "Could not write repaired copy to:\n" + dest.getFullPathName());
                    return;
                }
                SaveHistory::recordSave(dest);
                safeThis->inspectFile(dest);
                safeThis->appendReportLine("[SAVED COPY] Wrote rehabilitated setup to: " + dest.getFullPathName());
            });
    }

    void saveInPlace(bool loadAfterSave) {
        if (!hasValidJson || !currentFile.existsAsFile())
            return;

        if (hasPendingEdits()) {
            juce::var repaired;
            juce::String err;
            if (!buildRepairedRig(repaired, err)) {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon, "Repair Validation Failed", err);
                return;
            }

            // Snapshot pre-repair file into SaveHistory AND .bak via RigSerializer::save
            SaveHistory::recordSave(currentFile);
            const juce::String jsonOut = juce::JSON::toString(repaired);
            if (!RigSerializer::save(currentFile, jsonOut)) {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon, "Save Failed",
                    "Could not write repaired song file:\n" + currentFile.getFullPathName());
                return;
            }
            SaveHistory::recordSave(currentFile);
            LOG_INFO("RigDoctor rehabilitated song: " + currentFile.getFullPathName());

            if (!loadAfterSave) {
                inspectFile(currentFile);
                appendReportLine("[REPAIRED] Saved surgical repairs to '" + currentFile.getFileName() +
                                 "' (pre-repair snapshot preserved in Version History and .bak).");
                return;
            }
        }

        if (loadAfterSave) {
            const juce::File fileToLoad = currentFile;
            auto loadCb = actions.onLoadSong;
            auto closeCb = actions.onClose;
            if (closeCb)
                closeCb();
            if (loadCb)
                loadCb(fileToLoad);
        }
    }

    //==========================================================================
    // Isolated Dry-Run Plugin Tester
    //==========================================================================
    void startProbe(bool selectedOnly) {
        if (probeRunning.load(std::memory_order_acquire) || rows.empty())
            return;

        std::vector<int> indices;
        if (selectedOnly) {
            int sel = table.getSelectedRow();
            if (sel < 0 || sel >= (int)rows.size())
                return;
            indices.push_back(sel);
        } else {
            for (int i = 0; i < (int)rows.size(); ++i)
                indices.push_back(i);
        }

        if (unloadLiveToggle.getToggleState()) {
            engine.clearAllRacks();
            appendReportLine("[PROBE] Unloaded live mixer for clean-room plugin dry-run.");
        }

        probeStopFlag->store(false, std::memory_order_release);
        probeRunning.store(true, std::memory_order_release);
        updateFooterButtonStates();

        struct ProbeJob {
            int rowIndex = 0;
            juce::String location;
            juce::String name;
            juce::String path;
            juce::var pluginVar;
            bool fileExists = false;
            bool requiresMsgThread = false;
            RepairAction action = RepairAction::Keep;
        };

        std::vector<ProbeJob> jobs;
        for (int idx : indices) {
            const auto& r = rows[(size_t)idx];
            ProbeJob j;
            j.rowIndex = idx;
            j.location = r.locationLabel;
            j.name = r.pluginName;
            j.path = r.pluginPath;
            j.pluginVar = r.pluginVar;
            j.fileExists = r.fileExists;
            j.requiresMsgThread = r.requiresMessageThread;
            j.action = r.action;
            if (j.action == RepairAction::ResetPatch && j.pluginVar.isObject()) {
                // Clone var without state so dry-run tests what will actually load!
                auto parsed = juce::JSON::parse(juce::JSON::toString(j.pluginVar));
                if (auto* o = parsed.getDynamicObject())
                    o->setProperty("state", "");
                j.pluginVar = parsed;
            }
            jobs.push_back(std::move(j));
        }

        appendReportLine("[PROBE] Starting isolated dry-run of " + juce::String((int)jobs.size()) + " plugin(s)...");

        juce::Component::SafePointer<RigDoctorModal> safeThis(this);
        auto stopFlag = probeStopFlag;
        auto modalAlive = aliveToken;
        auto engAlive = engine.getAliveToken();
        FanfareEngine* engPtr = &engine;

        std::thread([safeThis, jobs = std::move(jobs), stopFlag, modalAlive, engAlive, engPtr]() {
            auto postRowUpdate = [safeThis, modalAlive](int rowIdx, ProbeState st, int ms, const juce::String& detail, const juce::String& logLine) {
                juce::MessageManager::callAsync([safeThis, modalAlive, rowIdx, st, ms, detail, logLine]() {
                    if (!modalAlive->load(std::memory_order_acquire) || safeThis == nullptr)
                        return;
                    if (rowIdx >= 0 && rowIdx < (int)safeThis->rows.size()) {
                        auto& r = safeThis->rows[(size_t)rowIdx];
                        r.probeState = st;
                        r.probeMs = ms;
                        r.probeDetail = detail;
                        safeThis->table.repaintRow(rowIdx);
                    }
                    if (logLine.isNotEmpty())
                        safeThis->appendReportLine(logLine);
                });
            };

            auto shouldStop = [&]() {
                return stopFlag->load(std::memory_order_acquire) ||
                       !modalAlive->load(std::memory_order_acquire) ||
                       (engAlive && !engAlive->load(std::memory_order_acquire));
            };

            for (size_t k = 0; k < jobs.size(); ++k) {
                if (shouldStop())
                    break;

                const auto& job = jobs[k];
                const juce::String stepTag = "[" + juce::String((int)k + 1) + "/" + juce::String((int)jobs.size()) + "] "
                                           + job.location + " " + job.name;

                if (job.action == RepairAction::Remove) {
                    postRowUpdate(job.rowIndex, ProbeState::Skipped, 0, "marked REMOVE",
                                  stepTag + " -> SKIPPED (marked REMOVE)");
                    continue;
                }
                if (!job.fileExists) {
                    postRowUpdate(job.rowIndex, ProbeState::BuildFailed, 0, "VST3 missing",
                                  stepTag + " -> MISSING VST3 (" + job.path + ")");
                    continue;
                }

                postRowUpdate(job.rowIndex, ProbeState::Testing, 0, "", "");
                const juce::int64 t0 = juce::Time::getMillisecondCounter();

                if (!tryAcquirePathBuildGate(job.path, 30000, shouldStop)) {
                    if (shouldStop())
                        break;
                    postRowUpdate(job.rowIndex, ProbeState::TimedOut, 0, "gate locked",
                                  stepTag + " -> LOCKED (an earlier build of this VST3 is still wedged)");
                    continue;
                }

                bool ok = false;
                bool timedOut = false;
                bool blankOkAfterFault = false;
                juce::String errMsg;

                if (job.requiresMsgThread) {
                    struct MsgProbeCtx {
                        FanfareEngine* eng = nullptr;
                        std::shared_ptr<std::atomic<bool>> engAlive;
                        juce::var pv;
                        juce::String path;
                        bool ok = false;
                        bool blankOk = false;
                        juce::String err;
                        juce::WaitableEvent done;
                        std::atomic<bool> abandoned{false};
                    };
                    auto ctx = std::make_shared<MsgProbeCtx>();
                    ctx->eng = engPtr;
                    ctx->engAlive = engAlive;
                    ctx->pv = job.pluginVar;
                    ctx->path = job.path;

                    juce::MessageManager::callAsync([ctx]() {
                        if (!ctx->abandoned.load(std::memory_order_acquire) &&
                            (!ctx->engAlive || ctx->engAlive->load(std::memory_order_acquire))) {
                            std::unique_ptr<juce::AudioPluginInstance> inst;
                            juce::String err;
                            ctx->ok = ctx->eng->buildPluginFromVar(ctx->pv, inst, err) && (bool)inst;
                            ctx->err = err;
                            if (inst) {
                                FanfareLog::safeExecutePluginCall([&]() {
                                    inst->releaseResources();
                                    inst.reset();
                                }, "RigDoctor STA release");
                            } else if (ctx->pv.getProperty("state", "").toString().isNotEmpty()) {
                                auto blank = juce::JSON::parse(juce::JSON::toString(ctx->pv));
                                if (auto* o = blank.getDynamicObject())
                                    o->setProperty("state", "");
                                std::unique_ptr<juce::AudioPluginInstance> inst2;
                                juce::String err2;
                                ctx->blankOk = ctx->eng->buildPluginFromVar(blank, inst2, err2) && (bool)inst2;
                                if (inst2) {
                                    FanfareLog::safeExecutePluginCall([&]() {
                                        inst2->releaseResources();
                                        inst2.reset();
                                    }, "RigDoctor STA blank release");
                                }
                            }
                        }
                        releasePathBuildGate(ctx->path);
                        ctx->done.signal();
                    });

                    bool signaled = false;
                    for (int w = 0; w < 150; ++w) { // 30s bound
                        if (ctx->done.wait(200)) {
                            signaled = true;
                            break;
                        }
                        if (shouldStop())
                            break;
                    }
                    if (!signaled) {
                        ctx->abandoned.store(true, std::memory_order_release);
                        timedOut = !shouldStop();
                    } else {
                        ok = ctx->ok;
                        blankOkAfterFault = ctx->blankOk;
                        errMsg = ctx->err;
                    }
                } else {
                    struct MtaProbeCtx {
                        FanfareEngine* eng = nullptr;
                        std::shared_ptr<std::atomic<bool>> engAlive;
                        juce::var pv;
                        juce::String path;
                        bool ok = false;
                        bool blankOk = false;
                        juce::String err;
                        std::atomic<bool> done{false};
                        std::atomic<bool> abandoned{false};
                    };
                    auto ctx = std::make_shared<MtaProbeCtx>();
                    ctx->eng = engPtr;
                    ctx->engAlive = engAlive;
                    ctx->pv = job.pluginVar;
                    ctx->path = job.path;

                    std::thread worker([ctx]() {
                        ctx->eng->applyBackgroundThreadTuning();
#ifdef _WIN32
                        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
                        if (!ctx->abandoned.load(std::memory_order_acquire) &&
                            (!ctx->engAlive || ctx->engAlive->load(std::memory_order_acquire))) {
                            std::unique_ptr<juce::AudioPluginInstance> inst;
                            juce::String err;
                            ctx->ok = ctx->eng->buildPluginFromVar(ctx->pv, inst, err) && (bool)inst;
                            ctx->err = err;
                            if (inst) {
                                FanfareLog::safeExecutePluginCall([&]() {
                                    inst->releaseResources();
                                    inst.reset();
                                }, "RigDoctor MTA release");
                            } else if (!ctx->abandoned.load(std::memory_order_acquire) &&
                                       ctx->pv.getProperty("state", "").toString().isNotEmpty()) {
                                auto blank = juce::JSON::parse(juce::JSON::toString(ctx->pv));
                                if (auto* o = blank.getDynamicObject())
                                    o->setProperty("state", "");
                                std::unique_ptr<juce::AudioPluginInstance> inst2;
                                juce::String err2;
                                ctx->blankOk = ctx->eng->buildPluginFromVar(blank, inst2, err2) && (bool)inst2;
                                if (inst2) {
                                    FanfareLog::safeExecutePluginCall([&]() {
                                        inst2->releaseResources();
                                        inst2.reset();
                                    }, "RigDoctor MTA blank release");
                                }
                            }
                        }
#ifdef _WIN32
                        ::CoUninitialize();
#endif
                        releasePathBuildGate(ctx->path);
                        ctx->done.store(true, std::memory_order_release);
                    });

                    int waits = 0;
                    while (!ctx->done.load(std::memory_order_acquire) && waits < 300 && !shouldStop()) {
                        juce::Thread::sleep(100);
                        ++waits;
                    }
                    if (!ctx->done.load(std::memory_order_acquire)) {
                        ctx->abandoned.store(true, std::memory_order_release);
                        worker.detach();
                        timedOut = !shouldStop();
                    } else {
                        worker.join();
                        ok = ctx->ok;
                        blankOkAfterFault = ctx->blankOk;
                        errMsg = ctx->err;
                    }
                }

                if (shouldStop())
                    break;

                const int elapsed = (int)(juce::Time::getMillisecondCounter() - t0);
                if (timedOut) {
                    poisonPath(job.path);
                    postRowUpdate(job.rowIndex, ProbeState::TimedOut, elapsed, "stalled >30s",
                                  stepTag + " -> STALLED (" + juce::String(elapsed / 1000) +
                                  "s). Recommendation: set Surgical Action to RESET PATCH or REMOVE.");
                } else if (ok) {
                    postRowUpdate(job.rowIndex, ProbeState::Passed, elapsed, "",
                                  stepTag + " (" + (job.requiresMsgThread ? "STA" : "MTA") +
                                  ") -> PASS in " + juce::String(elapsed) + " ms");
                } else if (blankOkAfterFault) {
                    postRowUpdate(job.rowIndex, ProbeState::PatchFault, elapsed, errMsg,
                                  stepTag + " -> PATCH FAULT (plugin binary works, saved state failed). Recommendation: click RESET.");
                } else {
                    postRowUpdate(job.rowIndex, ProbeState::BuildFailed, elapsed,
                                  errMsg.isEmpty() ? "load failed" : errMsg,
                                  stepTag + " -> FAILED (" + (errMsg.isEmpty() ? "unknown error" : errMsg) + ")");
                }

                // Brief breathing room so Windows message pump stays snappy
                juce::Thread::sleep(25);
            }

            const bool wasStopped = stopFlag->load(std::memory_order_acquire);
            juce::MessageManager::callAsync([safeThis, modalAlive, wasStopped]() {
                if (!modalAlive->load(std::memory_order_acquire) || safeThis == nullptr)
                    return;
                safeThis->probeRunning.store(false, std::memory_order_release);
                safeThis->refreshPoisonStatus();
                safeThis->updateFooterButtonStates();
                safeThis->appendReportLine(wasStopped ? "[PROBE] Dry-run stopped by user."
                                                      : "[PROBE] Isolated dry-run finished.");
            });
        }).detach();
    }

    void stopProbe() {
        probeStopFlag->store(true, std::memory_order_release);
    }

    FanfareEngine& engine;
    Actions actions;
    juce::File currentFile;
    juce::String originalJsonText;
    bool hasValidJson = false;
    bool originalFreshBuild = false;
    std::vector<PluginRow> rows;
    juce::StringArray missingMediaSummary;

    std::shared_ptr<std::atomic<bool>> aliveToken = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> probeStopFlag = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> probeRunning{false};

    juce::Label fileInfoLabel;
    juce::TextButton helpBtn, closeBtn, browseBtn, rescanBtn;
    juce::TextButton testAllBtn, testSelectedBtn, stopTestBtn, clearLocksBtn;
    juce::ToggleButton unloadLiveToggle;
    juce::TableListBox table;
    juce::TextEditor reportBox;
    juce::ToggleButton freshBuildToggle, clearMissingMediaToggle;
    juce::TextButton historyBtn, restoreBakBtn, resetEditsBtn;
    juce::TextButton saveCopyBtn, repairSaveBtn, repairLoadBtn;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RigDoctorModal)
};

} // namespace Fanfare

