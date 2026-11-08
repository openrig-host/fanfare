#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <utility>
#include "FanfareConstants.h"
#include "RigLibrary.h"

// SaveHistory.h - append-only version history for songs, queue maps, and strip presets.
// Every save writes an atomic timestamped snapshot under:
//   %APPDATA%/Fanfare/backups/history/<kind>/<subfolders>/<name>/<YYYYMMDD-HHMMSS>[_NN].<ext>
// Deduplicated by chunked byte comparison against the newest snapshot so unchanged
// saves never create noise. Pre-load snapshots run off the message thread so
// live stage song switches never block on multi-MB file reads, and are drained
// cleanly at shutdown before process termination.
namespace SaveHistory {

inline juce::CriticalSection& historyLock() {
    static juce::CriticalSection cs;
    return cs;
}

inline juce::CriticalSection& restoreLock() {
    static juce::CriticalSection cs;
    return cs;
}

struct AsyncSnapshotTracker {
    std::mutex mutex;
    std::condition_variable cv;
    int inFlight = 0;
    std::atomic<bool> shuttingDown{false};
};

inline AsyncSnapshotTracker& asyncTracker() {
    static AsyncSnapshotTracker tracker;
    return tracker;
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

// Parses YYYYMMDD-HHMMSS (and optional same-second suffix _NN or legacy -N)
// into (epochMs, sameSecondSeq) so chronological sorting is exact.
inline std::pair<juce::int64, int> parseVersionSortKey(const juce::File& versionFile) {
    const auto stem = versionFile.getFileNameWithoutExtension();
    if (stem.length() >= 15 && stem[8] == '-') {
        const int yr = stem.substring(0, 4).getIntValue();
        const int mo = stem.substring(4, 6).getIntValue();
        const int dy = stem.substring(6, 8).getIntValue();
        const int hr = stem.substring(9, 11).getIntValue();
        const int mn = stem.substring(11, 13).getIntValue();
        const int sc = stem.substring(13, 15).getIntValue();
        if (yr >= 2020 && mo >= 1 && mo <= 12 && dy >= 1 && dy <= 31) {
            int seq = 0;
            if (stem.length() > 16 && (stem[15] == '_' || stem[15] == '-'))
                seq = juce::jmax(0, stem.substring(16).getIntValue());
            const juce::Time t(yr, mo - 1, dy, hr, mn, sc, 0, true);
            return { t.toMilliseconds(), seq };
        }
    }
    return { versionFile.getLastModificationTime().toMilliseconds(), 0 };
}

// Parses YYYYMMDD-HHMMSS from snapshot filename so age/pruning never depend on
// copied file mtimes. Falls back to getLastModificationTime() if non-standard.
inline juce::Time parseVersionTime(const juce::File& versionFile) {
    return juce::Time(parseVersionSortKey(versionFile).first);
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

struct NewestVersionFirst {
    int compareElements(const juce::File& a, const juce::File& b) const {
        const auto ka = parseVersionSortKey(a);
        const auto kb = parseVersionSortKey(b);
        if (kb.first != ka.first)
            return (kb.first > ka.first) ? 1 : -1;
        if (kb.second != ka.second)
            return (kb.second > ka.second) ? 1 : -1;
        return b.getFileName().compareNatural(a.getFileName());
    }
};

inline juce::Array<juce::File> findSnapshotsUnlocked(const juce::File& dir, const juce::String& ext) {
    juce::Array<juce::File> out;
    if (!dir.isDirectory())
        return out;
    juce::Array<juce::File> raw;
    dir.findChildFiles(raw, juce::File::findFiles, false, "*" + ext);
    for (const auto& candidate : raw) {
        const auto name = candidate.getFileName();
        if (name.endsWithIgnoreCase(".tmp") || name.endsWithIgnoreCase(".temp") || name.startsWithChar('.'))
            continue;
        out.add(candidate);
    }
    NewestVersionFirst cmp;
    out.sort(cmp, false);
    return out;
}

inline bool filesIdenticalChunked(const juce::File& a, const juce::File& b) {
    const juce::int64 sizeA = a.getSize();
    const juce::int64 sizeB = b.getSize();
    if (sizeA < 0 || sizeA != sizeB)
        return false;
    if (sizeA == 0)
        return a.existsAsFile() && b.existsAsFile();

    juce::FileInputStream streamA(a);
    juce::FileInputStream streamB(b);
    if (!streamA.openedOk() || !streamB.openedOk())
        return false;

    constexpr size_t kChunkBytes = 65536;
    char bufA[kChunkBytes];
    char bufB[kChunkBytes];
    juce::int64 remaining = sizeA;
    while (remaining > 0) {
        const int toRead = (int)juce::jmin<juce::int64>(remaining, (juce::int64)kChunkBytes);
        const int readA = streamA.read(bufA, toRead);
        const int readB = streamB.read(bufB, toRead);
        if (readA != toRead || readB != toRead)
            return false;
        if (std::memcmp(bufA, bufB, (size_t)toRead) != 0)
            return false;
        remaining -= toRead;
    }
    return true;
}

// Newest first (sorted by parsed timestamp + same-second sequence).
inline juce::Array<juce::File> versions(const juce::File& f) {
    const juce::ScopedLock sl(historyLock());
    return findSnapshotsUnlocked(dirFor(f), f.getFileExtension());
}

inline void pruneUnlocked(const juce::File& f, int keepNewest = 20, int keepDays = 7) {
    const auto all = findSnapshotsUnlocked(dirFor(f), f.getFileExtension());
    const auto cutoff = juce::Time::getCurrentTime() - juce::RelativeTime::days(keepDays);
    for (int i = 0; i < all.size(); ++i) {
        if (i < keepNewest)
            continue;
        if (parseVersionTime(all.getReference(i)) > cutoff)
            continue;
        all.getReference(i).deleteFile();
    }
}

inline juce::File makeUniqueSnapshotTargetUnlocked(const juce::File& dir, const juce::String& ext) {
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
    auto target = dir.getChildFile(stamp + ext);
    int n = 1;
    // Use '_' suffix ('_' > '.' in ASCII) AND parsed sequence tiebreak so same-second
    // snapshots always sort newer than the base timestamp file.
    while (target.existsAsFile())
        target = dir.getChildFile(stamp + "_" + juce::String(n++).paddedLeft('0', 2) + ext);
    return target;
}

// Records current on-disk file into history if it differs from the latest snapshot.
// Heavy file reads and staging copy run outside historyLock() so message-thread saves
// never stall behind background snapshots of large rigs.
inline bool recordSave(const juce::File& f) {
    if (!f.existsAsFile())
        return false;

    const auto dir = dirFor(f);
    const auto ext = f.getFileExtension();

    juce::File newestBefore;
    {
        const juce::ScopedLock sl(historyLock());
        const auto existing = findSnapshotsUnlocked(dir, ext);
        if (!existing.isEmpty())
            newestBefore = existing.getReference(0);
    }

    if (newestBefore != juce::File{} && filesIdenticalChunked(f, newestBefore))
        return false;

    if (!dir.createDirectory())
        return false;

    // Stage into a unique non-matching temp file inside the history directory first
    // so partial copies can never appear in versions() even if interrupted.
    const juce::String tempName = ".stage_" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()) + ".snapshot_tmp";
    juce::File stagedTemp = dir.getChildFile(tempName);
    if (!f.copyFileTo(stagedTemp)) {
        stagedTemp.deleteFile();
        return false;
    }

    const juce::int64 expectedSize = stagedTemp.getSize();
    if (expectedSize < 0) {
        stagedTemp.deleteFile();
        return false;
    }

    {
        const juce::ScopedLock sl(historyLock());
        const auto existing = findSnapshotsUnlocked(dir, ext);
        if (!existing.isEmpty() && filesIdenticalChunked(stagedTemp, existing.getReference(0))) {
            stagedTemp.deleteFile();
            return false;
        }

        const auto target = makeUniqueSnapshotTargetUnlocked(dir, ext);
        if (!stagedTemp.moveFileTo(target)) {
            stagedTemp.deleteFile();
            return false;
        }
        pruneUnlocked(f);
    }
    return true;
}

// Atomic, lock-coordinated restore:
//   1. Snapshots the current on-disk targetFile (if present) into history.
//   2. Copies versionFile to a sibling temp file next to targetFile and atomically
//      replaces targetFile via MOVEFILE_REPLACE_EXISTING — never deleting targetFile
//      before the replacement bytes are safely on disk (fixes H13 + M8).
//   3. Records the restored targetFile in history.
inline bool restoreVersion(const juce::File& versionFile, const juce::File& targetFile, juce::String& outError) {
    const juce::ScopedLock rl(restoreLock());

    if (!versionFile.existsAsFile()) {
        outError = "Selected snapshot file no longer exists on disk.";
        return false;
    }

    const juce::int64 srcSize = versionFile.getSize();
    if (srcSize <= 0) {
        outError = "Selected snapshot file is empty or unreadable.";
        return false;
    }

    if (targetFile.existsAsFile())
        recordSave(targetFile);

    auto parentDir = targetFile.getParentDirectory();
    if (!parentDir.createDirectory()) {
        outError = "Could not access destination folder:\n" + parentDir.getFullPathName();
        return false;
    }

    juce::TemporaryFile tempTarget(targetFile);
    if (!versionFile.copyFileTo(tempTarget.getFile()) || tempTarget.getFile().getSize() != srcSize) {
        tempTarget.deleteTemporaryFile();
        outError = "Could not write restored file (disk full or permission denied). Your existing file was left untouched.";
        return false;
    }

    if (!tempTarget.overwriteTargetFileWithTemporary()) {
        tempTarget.deleteTemporaryFile();
        outError = "Could not replace destination file (it may be locked by another program). Your existing file was left untouched.";
        return false;
    }

    recordSave(targetFile);
    return true;
}

// Non-blocking background snapshot used before loading a song/strip on stage.
inline void snapshotBeforeLoadAsync(const juce::File& f) {
    if (!f.existsAsFile())
        return;

    auto& tracker = asyncTracker();
    if (tracker.shuttingDown.load(std::memory_order_acquire))
        return;

    {
        std::lock_guard<std::mutex> lk(tracker.mutex);
        if (tracker.shuttingDown.load(std::memory_order_relaxed))
            return;
        ++tracker.inFlight;
    }

    const bool launched = juce::Thread::launch([f]() {
        struct Guard {
            ~Guard() {
                auto& tr = asyncTracker();
                {
                    std::lock_guard<std::mutex> lk(tr.mutex);
                    --tr.inFlight;
                }
                tr.cv.notify_all();
            }
        } guard;

        const juce::ScopedLock rl(restoreLock());
        recordSave(f);
    });

    if (!launched) {
        {
            std::lock_guard<std::mutex> lk(tracker.mutex);
            --tracker.inFlight;
        }
        tracker.cv.notify_all();
    }
}

// Drains any in-flight background snapshots before process termination.
inline void waitForPendingSnapshots(int timeoutMs = 2500) {
    auto& tracker = asyncTracker();
    tracker.shuttingDown.store(true, std::memory_order_release);
    std::unique_lock<std::mutex> lk(tracker.mutex);
    tracker.cv.wait_for(lk, std::chrono::milliseconds(juce::jmax(0, timeoutMs)), [&tracker] {
        return tracker.inFlight <= 0;
    });
}

} // namespace SaveHistory
