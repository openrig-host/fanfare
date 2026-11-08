#pragma once

#include <JuceHeader.h>
#include <cstring>
#include "FanfareConstants.h"
#include "RigLibrary.h"

// SaveHistory.h - append-only version history for songs, queue maps, and strip presets.
// Every save writes a timestamped snapshot under:
//   %APPDATA%/Fanfare/backups/history/<kind>/<subfolders>/<name>/<YYYYMMDD-HHMMSS>.<ext>
// Deduplicated by raw byte comparison against the newest snapshot so unchanged
// saves never create noise. Pre-load snapshots run off the message thread so
// live stage song switches never block on multi-MB file reads.
namespace SaveHistory {

inline juce::CriticalSection& historyLock() {
    static juce::CriticalSection cs;
    return cs;
}

inline juce::File historyRoot() {
    return FanfareConstants::getBackupsDirectory().getChildFile("history");
}

inline juce::String kindFor(const juce::File& f) {
    const auto parent = f.getParentDirectory();
    const auto songs  = Fanfare::RigLibrary::getSongsDirectory();
    const auto sets   = Fanfare::RigLibrary::getSetsDirectory();
    const auto strips = Fanfare::RigLibrary::getPresetsDirectory();
    if (parent == songs || parent.isAChildOf(songs))
        return "songs";
    if (parent == sets || parent.isAChildOf(sets))
        return "sets";
    if (parent == strips || parent.isAChildOf(strips))
        return "strips";
    return "misc";
}

inline juce::File dirFor(const juce::File& f) {
    const auto kind = kindFor(f);
    juce::File base;
    if (kind == "songs")
        base = Fanfare::RigLibrary::getSongsDirectory();
    else if (kind == "sets")
        base = Fanfare::RigLibrary::getSetsDirectory();
    else if (kind == "strips")
        base = Fanfare::RigLibrary::getPresetsDirectory();

    auto dir = historyRoot().getChildFile(kind);
    if (base != juce::File{} && f.getParentDirectory() != base &&
        f.getParentDirectory().isAChildOf(base)) {
        const auto rel = f.getParentDirectory().getRelativePathFrom(base);
        if (rel.isNotEmpty() && rel != ".")
            dir = dir.getChildFile(rel);
    }
    return dir.getChildFile(f.getFileNameWithoutExtension());
}

// Parses YYYYMMDD-HHMMSS from snapshot filename so age/pruning never depend on
// copied file mtimes. Falls back to getLastModificationTime() if non-standard.
inline juce::Time parseVersionTime(const juce::File& versionFile) {
    const auto stem = versionFile.getFileNameWithoutExtension();
    if (stem.length() >= 15 && stem[8] == '-') {
        const int yr  = stem.substring(0, 4).getIntValue();
        const int mo  = stem.substring(4, 6).getIntValue();
        const int dy  = stem.substring(6, 8).getIntValue();
        const int hr  = stem.substring(9, 11).getIntValue();
        const int mn  = stem.substring(11, 13).getIntValue();
        const int sc  = stem.substring(13, 15).getIntValue();
        if (yr >= 2020 && mo >= 1 && mo <= 12 && dy >= 1 && dy <= 31)
            return juce::Time(yr, mo - 1, dy, hr, mn, sc, 0, true);
    }
    return versionFile.getLastModificationTime();
}

// Human-readable timestamp ("2026-11-07  20:15:04") from snapshot filename.
inline juce::String describeVersion(const juce::File& versionFile) {
    const auto stem = versionFile.getFileNameWithoutExtension();
    if (stem.length() >= 15 && stem[8] == '-') {
        return stem.substring(0, 4) + "-" + stem.substring(4, 6) + "-" +
               stem.substring(6, 8) + "  " + stem.substring(9, 11) + ":" +
               stem.substring(11, 13) + ":" + stem.substring(13, 15);
    }
    return versionFile.getLastModificationTime().formatted("%Y-%m-%d  %H:%M:%S");
}

// Newest first (timestamp filenames sort lexicographically = chronologically).
inline juce::Array<juce::File> versions(const juce::File& f) {
    const juce::ScopedLock sl(historyLock());
    juce::Array<juce::File> out;
    const auto dir = dirFor(f);
    if (!dir.isDirectory())
        return out;
    dir.findChildFiles(out, juce::File::findFiles, false, "*" + f.getFileExtension());
    struct NewestFirst {
        int compareElements(const juce::File& a, const juce::File& b) const {
            return b.getFileName().compareNatural(a.getFileName());
        }
    } cmp;
    out.sort(cmp, false);
    return out;
}

inline void pruneUnlocked(const juce::File& f, int keepNewest = 20, int keepDays = 7) {
    juce::Array<juce::File> all;
    const auto dir = dirFor(f);
    if (!dir.isDirectory())
        return;
    dir.findChildFiles(all, juce::File::findFiles, false, "*" + f.getFileExtension());
    struct NewestFirst {
        int compareElements(const juce::File& a, const juce::File& b) const {
            return b.getFileName().compareNatural(a.getFileName());
        }
    } cmp;
    all.sort(cmp, false);

    const auto cutoff = juce::Time::getCurrentTime() - juce::RelativeTime::days(keepDays);
    for (int i = 0; i < all.size(); ++i) {
        if (i < keepNewest)
            continue;
        if (parseVersionTime(all.getReference(i)) > cutoff)
            continue;
        all.getReference(i).deleteFile();
    }
}

// Records current on-disk file into history if it differs from the latest snapshot.
inline bool recordSave(const juce::File& f) {
    if (!f.existsAsFile())
        return false;

    const juce::ScopedLock sl(historyLock());
    const auto dir = dirFor(f);
    if (dir.isDirectory()) {
        juce::Array<juce::File> all;
        dir.findChildFiles(all, juce::File::findFiles, false, "*" + f.getFileExtension());
        struct NewestFirst {
            int compareElements(const juce::File& a, const juce::File& b) const {
                return b.getFileName().compareNatural(a.getFileName());
            }
        } cmp;
        all.sort(cmp, false);

        if (!all.isEmpty()) {
            const auto& newest = all.getReference(0);
            const auto sz = f.getSize();
            if (sz == newest.getSize()) {
                juce::MemoryBlock a, b;
                if (f.loadFileAsData(a) && newest.loadFileAsData(b) &&
                    a.getSize() == b.getSize() &&
                    (a.getSize() == 0 || std::memcmp(a.getData(), b.getData(), a.getSize()) == 0)) {
                    return false;
                }
            }
        }
    }

    dir.createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
    auto target = dir.getChildFile(stamp + f.getFileExtension());
    int n = 1;
    while (target.existsAsFile())
        target = dir.getChildFile(stamp + "-" + juce::String(n++) + f.getFileExtension());

    if (!f.copyFileTo(target))
        return false;
    pruneUnlocked(f);
    return true;
}

// Non-blocking background snapshot used before loading a song/strip on stage.
inline void snapshotBeforeLoadAsync(const juce::File& f) {
    if (!f.existsAsFile())
        return;
    juce::Thread::launch([f]() {
        recordSave(f);
    });
}

} // namespace SaveHistory

