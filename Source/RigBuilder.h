#pragma once

#include <JuceHeader.h>
#include <thread>
#include <mutex>
#include <map>
#include <set>
#include <condition_variable>
#include <chrono>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <objbase.h>
#endif
#include "Logger.h"
#include "PluginBuildGate.h"
#include "FanfareEngine.h"

namespace Fanfare {

// Shared, refcounted state for off-thread -> message-thread plugin builds, so
// the async callback can NEVER dangle if the transitioner thread is torn down
// (e.g. stopTransition) before the message thread runs it. The captured
// shared_ptr keeps this state alive for the lifetime of the callback.


/**
    RigBuilder
    ----------
    Builds a rig's plugin instances OFF the audio and message threads.

    Strategy (mirrors the engine's existing reuse-by-path logic, just moved off
    the hot threads):
      - Snapshot the live rack's plugin paths.
      - For each plugin location in the new rig whose path DIFFERS from the
        live rack (or has no live instance), build + configure + prepare +
        restore-state the instance on this worker thread, then run a silent
        processBlock validation.
      - Matching paths are reused untouched (zero-cost, zero-risk).
      - Built instances are pushed into the engine's staging cache, keyed by
        rack location, for applyRig() to consume later under the callback lock.

    Any failure aborts the build; the live rack is never touched by this class.
    Failed entries are returned so RigTransitioner can retry them on the
    message thread (some VST3s require message-thread instantiation).
*/
class RigBuilder {
public:
    struct FailedEntry {
        juce::String key;
        juce::var pluginVar;
        juce::String reason;
    };

    struct Result {
        bool ok = true;
        juce::String error;
        int builtCount = 0;
        int reusedCount = 0;
        std::vector<FailedEntry> failedEntries;
    };

    static Result build(FanfareEngine &engine, const juce::var &rig,
                        std::function<void(const juce::String&)> onProgress = nullptr,
                        bool isPreload = false,
                        std::function<bool()> shouldAbort = nullptr) {
        Result r;
        if (!rig.isObject())
            return r; // nothing to build

        auto snap = engine.snapshotPluginPaths();

        struct BuildEntry {
            juce::var pv;
            juce::String key;
            juce::String label;
            juce::String name;
            juce::String path;
            int slotIdx = -1, chainIdx = -1;        // buildOne when slotIdx >= 0
            bool isFoh = false; int masterIdx = -1; // buildMaster otherwise
        };
        std::vector<BuildEntry> entries;

        auto entryName = [](const juce::var &pv) {
            juce::String name = pv.getProperty("name", "").toString();
            if (name.isEmpty())
                name = juce::File(pv.getProperty("path", "").toString())
                           .getFileNameWithoutExtension();
            return name;
        };

        if (auto *chans = rig.getProperty("channels", juce::var()).getArray()) {
            int n = std::min((int)chans->size(), engine.getNumSlots());
            for (int i = 0; i < n; ++i) {
                auto cv = chans->getReference(i);
                if (auto *chain = cv.getProperty("chain", juce::var()).getArray()) {
                    for (int p = 0; p < (int)chain->size() && p < 3; ++p) {
                        auto pv = chain->getReference(p);
                        if (!pv.isObject())
                            continue;
                        const juce::String rawPath = pv.getProperty("path", "").toString();
                        if (rawPath.isEmpty())
                            continue;
                        BuildEntry e;
                        e.pv = pv;
                        e.key = FanfareEngine::stagingKeyFor(i, p, true);
                        e.slotIdx = i;
                        e.chainIdx = p;
                        e.label = "Slot " + juce::String(i + 1) +
                                  " chain " + juce::String(p + 1);
                        e.name = entryName(pv);
                        e.path = engine.normalizePath(rawPath);
                        entries.push_back(std::move(e));
                    }
                }
            }
        }

        // Aux return strips (slotIdx >= 100 convention)
        if (auto *auxArr = rig.getProperty("auxReturns", juce::var()).getArray()) {
            for (int a = 0; a < auxArr->size(); ++a) {
                auto av = auxArr->getReference(a);
                if (auto *chain = av.getProperty("chain", juce::var()).getArray()) {
                    for (int p = 0; p < (int)chain->size() && p < 3; ++p) {
                        auto pv = chain->getReference(p);
                        if (!pv.isObject())
                            continue;
                        const juce::String rawPath = pv.getProperty("path", "").toString();
                        if (rawPath.isEmpty())
                            continue;
                        BuildEntry e;
                        e.pv = pv;
                        e.key = FanfareEngine::stagingKeyFor(100 + a, p, true);
                        e.slotIdx = 100 + a;
                        e.chainIdx = p;
                        e.label = "Aux " + juce::String(a + 1) +
                                  " chain " + juce::String(p + 1);
                        e.name = entryName(pv);
                        e.path = engine.normalizePath(rawPath);
                        entries.push_back(std::move(e));
                    }
                }
            }
        }

        auto collectMasterFx = [&](const char *prop, bool isFoh) {
            if (auto *fx = rig.getProperty(prop, juce::var()).getArray()) {
                for (int p = 0; p < (int)fx->size() && p < 3; ++p) {
                    auto pv = fx->getReference(p);
                    if (!pv.isObject())
                        continue;
                    const juce::String rawPath = pv.getProperty("path", "").toString();
                    if (rawPath.isEmpty())
                        continue;
                    BuildEntry e;
                    e.pv = pv;
                    e.isFoh = isFoh;
                    e.masterIdx = p;
                    e.label = juce::String(isFoh ? "FOH master " : "IEM master ") +
                              juce::String(p + 1);
                    e.name = entryName(pv);
                    e.path = engine.normalizePath(rawPath);
                    entries.push_back(std::move(e));
                }
            }
        };
        collectMasterFx("fohFx", true);
        collectMasterFx("iemFx", false);

        // Partition into TWO non-overlapping phases to prevent cross-thread AB-BA
        // deadlocks (Windows PE Loader Lock / vendor singleton mutex vs Win32 SendMessage):
        //   Phase 1: STA Message-Thread plugins (NI Kontakt/Raum/Replika/Massive,
        //            Spectrasonics, Toontrack, HALion) — built sequentially one at a
        //            time with Win32 message-pump breathing room and ZERO concurrent
        //            background LoadLibrary/SendMessage workers.
        //   Phase 2: Off-thread plugins (UVI Workstation, Blue3, Hammond B-3X,
        //            Jun-6 V, Roland XV-5080/ZENOLOGY, etc.) — built in parallel
        //            (round-robin by path) while the message thread is 100% idle and
        //            freely pumping messages (required by UVI Workstation).
        std::vector<BuildEntry> msgThreadEntries;
        std::vector<BuildEntry> offThreadEntries;
        msgThreadEntries.reserve(entries.size());
        offThreadEntries.reserve(entries.size());

        for (auto &e : entries) {
            if (Fanfare::pluginRequiresMessageThread(e.path))
                msgThreadEntries.push_back(std::move(e));
            else
                offThreadEntries.push_back(std::move(e));
        }

        // Round-robin the off-thread dispatch order by plugin path
        {
            std::map<juce::String, std::deque<int>> byPath;
            for (int idx = 0; idx < (int)offThreadEntries.size(); ++idx) {
                juce::String k = offThreadEntries[idx].path.isEmpty()
                                     ? "@none@" + juce::String(idx)
                                     : offThreadEntries[idx].path;
                byPath[k].push_back(idx);
            }
            std::vector<BuildEntry> ordered;
            ordered.reserve(offThreadEntries.size());
            bool any = true;
            while (any) {
                any = false;
                for (auto &kv : byPath) {
                    if (!kv.second.empty()) {
                        ordered.push_back(std::move(offThreadEntries[kv.second.front()]));
                        kv.second.pop_front();
                        any = true;
                    }
                }
            }
            offThreadEntries.swap(ordered);
        }

        const int totalEntries = (int)(msgThreadEntries.size() + offThreadEntries.size());
        std::atomic<int> completedCount{0};

        logToFile("TRACE: RigBuilder two-phase dispatch: " +
                  juce::String(totalEntries) + " plugin(s) total (" +
                  juce::String((int)msgThreadEntries.size()) + " message-thread STA, " +
                  juce::String((int)offThreadEntries.size()) + " off-thread worker).");

        std::vector<std::unique_ptr<Result>> results;
        results.reserve((size_t)totalEntries);

        // --- PHASE 1: Sequential Message-Thread Builds ---
        for (size_t i = 0; i < msgThreadEntries.size(); ++i) {
            if (shouldAbort && shouldAbort())
                break;
            const auto &e = msgThreadEntries[i];
            if (onProgress) {
                onProgress("Loading [" + juce::String(completedCount.load() + 1) + "/" +
                           juce::String(totalEntries) + "] " + e.name +
                           " (" + e.label + ")...");
            }
            auto res = std::make_unique<Result>();
            if (e.slotIdx >= 0)
                buildOne(engine, e.pv, e.key, snap.slotChains, snap.auxChains,
                         e.slotIdx, e.chainIdx, e.label, *res, isPreload, shouldAbort);
            else
                buildMaster(engine, e.pv, e.isFoh, e.masterIdx,
                            e.isFoh ? snap.fohChain : snap.iemChain,
                            e.label, *res, isPreload, shouldAbort);
            results.push_back(std::move(res));
            completedCount.fetch_add(1, std::memory_order_relaxed);
            // Let the message loop paint LoadingOverlay & process pending Win32 messages
            if (! (shouldAbort && shouldAbort()))
                juce::Thread::sleep(20);
        }

        // --- PHASE 2: Off-Thread Worker Builds (Message Thread Free & Pumping) ---
        if (!offThreadEntries.empty() && !(shouldAbort && shouldAbort())) {
            struct WorkerTrack {
                juce::String name;
                juce::String label;
                std::atomic<bool> active{false};
                std::atomic<bool> finished{false};
                std::atomic<juce::int64> startMs{0};
            };
            std::vector<std::unique_ptr<WorkerTrack>> tracks(offThreadEntries.size());
            std::vector<std::unique_ptr<Result>> offResults(offThreadEntries.size());
            std::vector<std::thread> workers;
            workers.reserve(offThreadEntries.size());

            for (size_t idx = 0; idx < offThreadEntries.size(); ++idx) {
                tracks[idx] = std::make_unique<WorkerTrack>();
                tracks[idx]->name = offThreadEntries[idx].name;
                tracks[idx]->label = offThreadEntries[idx].label;
                offResults[idx] = std::make_unique<Result>();

                BuildEntry e = offThreadEntries[idx];
                Result *res = offResults[idx].get();
                WorkerTrack *trk = tracks[idx].get();

                workers.emplace_back([&engine, &snap, e, res, trk, &completedCount,
                                      isPreload, shouldAbort]() {
                    engine.applyBackgroundThreadTuning();
                    if (shouldAbort && shouldAbort()) {
                        trk->finished.store(true, std::memory_order_release);
                        return;
                    }
                    trk->startMs.store(juce::Time::currentTimeMillis(), std::memory_order_relaxed);
                    trk->active.store(true, std::memory_order_release);
                    if (e.slotIdx >= 0)
                        buildOne(engine, e.pv, e.key, snap.slotChains,
                                 snap.auxChains, e.slotIdx, e.chainIdx, e.label,
                                 *res, isPreload, shouldAbort);
                    else
                        buildMaster(engine, e.pv, e.isFoh, e.masterIdx,
                                    e.isFoh ? snap.fohChain : snap.iemChain,
                                    e.label, *res, isPreload, shouldAbort);
                    trk->active.store(false, std::memory_order_release);
                    completedCount.fetch_add(1, std::memory_order_relaxed);
                    trk->finished.store(true, std::memory_order_release);
                });
            }

            // Live progress & stall telemetry loop while off-thread workers run
            for (;;) {
                bool allDone = true;
                juce::String activeDesc;
                int activeCount = 0;
                const juce::int64 nowMs = juce::Time::currentTimeMillis();
                for (const auto &trk : tracks) {
                    if (!trk->finished.load(std::memory_order_acquire)) {
                        allDone = false;
                        if (trk->active.load(std::memory_order_acquire)) {
                            ++activeCount;
                            if (activeCount <= 2) {
                                if (activeDesc.isNotEmpty())
                                    activeDesc += ", ";
                                activeDesc += trk->name + " (" + trk->label + ")";
                                const juce::int64 elapsedSec =
                                    (nowMs - trk->startMs.load(std::memory_order_relaxed)) / 1000;
                                if (elapsedSec >= 3)
                                    activeDesc += " [" + juce::String(elapsedSec) + "s]";
                            }
                        }
                    }
                }
                if (allDone)
                    break;
                if (onProgress && activeDesc.isNotEmpty()) {
                    juce::String tail = (activeCount > 2)
                        ? (" +" + juce::String(activeCount - 2) + " more")
                        : juce::String();
                    const int currentStep = juce::jmin(totalEntries, completedCount.load() + 1);
                    onProgress("Loading [" + juce::String(currentStep) + "/" +
                               juce::String(totalEntries) + "] " + activeDesc + tail);
                }
                juce::Thread::sleep(250);
            }

            for (auto &t : workers)
                t.join();
            for (auto &res : offResults)
                results.push_back(std::move(res));
        }

        // --- Merge the per-entry results
        for (auto &res : results) {
            r.builtCount += res->builtCount;
            r.reusedCount += res->reusedCount;
            if (!res->failedEntries.empty()) {
                r.ok = false;
                if (r.error.isNotEmpty())
                    r.error += "; ";
                r.error += res->error;
                r.failedEntries.insert(r.failedEntries.end(),
                                       res->failedEntries.begin(),
                                       res->failedEntries.end());
            }
        }
        return r;
    }

    // Retry building a single failed entry on the message thread. Pushes the
    // result into the staging cache on success.
    static bool rebuildOnMessageThread(FanfareEngine &engine,
                                       const FailedEntry &entry,
                                       juce::String &error) {
        logToFile("TRACE: rebuildOnMessageThread starting for key: " + entry.key);
        std::unique_ptr<juce::AudioPluginInstance> inst;
        if (!engine.buildPluginFromVar(entry.pluginVar, inst, error) || !inst) {
            logToFile("TRACE: rebuildOnMessageThread failed to build for key: " + entry.key + ", error: " + error);
            return false;
        }
        if (!engine.validatePluginInstance(*inst)) {
            error = "silent processBlock validation failed";
            logToFile("TRACE: rebuildOnMessageThread validation failed for key: " + entry.key);
            return false;
        }
        engine.pushStagedPlugin(entry.key, std::move(inst));
        logToFile("TRACE: rebuildOnMessageThread succeeded and staged key: " + entry.key);
        return true;
    }

private:
    static bool buildOne(FanfareEngine &engine, const juce::var &pv,
                         const juce::String &key,
                         const std::vector<juce::StringArray> &slotPaths,
                         const std::vector<juce::StringArray> &auxPaths,
                         int slotIdx, int chainIdx, const juce::String &label,
                         Result &r, bool isPreload = false,
                         std::function<bool()> shouldAbort = nullptr) {
        if (shouldAbort && shouldAbort())
            return false;
        if (isPreload ? engine.hasPreloadedPlugin(key) : engine.stagingHasKey(key)) {
            logToFile("TRACE: buildOne " + label + " already staged/preloaded, skipping build.");
            return true;
        }

        juce::String newPath = engine.normalizePath(pv.getProperty("path", "").toString());
        if (newPath.isEmpty())
            return true; // empty slot, nothing to build

        juce::String curPath;
        if (slotIdx >= 100) {
            int auxIdx = slotIdx - 100;
            if (auxIdx < (int)auxPaths.size() && chainIdx < auxPaths[auxIdx].size())
                curPath = auxPaths[auxIdx][chainIdx];
        } else if (slotIdx < (int)slotPaths.size() && chainIdx < slotPaths[slotIdx].size()) {
            curPath = slotPaths[slotIdx][chainIdx];
        }

        logToFile("TRACE: buildOne " + label + " (Key: " + key + "). curPath: " + curPath + ", newPath: " + newPath);

        // Reuse-by-path: keep the live instance for same-path plugins.
        // This avoids creating a second concurrent instance of single-instance plugins (Kontakt, Roland Zenology/XV-5080, Super 8)
        // which block or crash in their internal singleton lock when instantiated concurrently.
        if (curPath == newPath) {
            logToFile("TRACE: buildOne " + label + " reusing existing plugin instance (curPath == newPath): " + newPath);
            ++r.reusedCount;
            return true;
        }

        if (Fanfare::isPathPoisoned(newPath)) {
            logToFile("WARNING: buildOne " + label + " skipped — this plugin path already stalled past its timeout this session (" + newPath + "). Fix the offending multi (e.g. missing samples in Kontakt) or restart to retry.");
            r.ok = false;
            r.error = label + ": skipped (plugin stalled earlier this session)";
            r.failedEntries.push_back({key, pv, "stalled earlier this session"});
            engine.markStagedBuildFailed(key);
            return false;
        }

        const bool requiresMessageThread = Fanfare::pluginRequiresMessageThread(newPath);

        std::unique_ptr<juce::AudioPluginInstance> inst;
        juce::String err;
        bool ok = false;

        if (!requiresMessageThread) {
            // Serialize per path: wait out any earlier build of this same
            // plugin (possibly an abandoned one still restoring state).
            if (!Fanfare::tryAcquirePathBuildGate(newPath, 120000, shouldAbort)) {
                if (shouldAbort && shouldAbort())
                    return false;
                logToFile("WARNING: buildOne " + label + " skipped — previous build of " + newPath + " is still wedged.");
                r.ok = false;
                r.error = label + ": previous plugin build still wedged";
                r.failedEntries.push_back({key, pv, "previous build still wedged"});
                engine.markStagedBuildFailed(key);
                return false;
            }
            logToFile("TRACE: buildOne " + label + " building off-thread...");
            struct BuildThreadContext {
                FanfareEngine* engine;
                std::shared_ptr<std::atomic<bool>> engineAlive;
                juce::var pluginVar;
                std::unique_ptr<juce::AudioPluginInstance> inst;
                juce::String err;
                bool ok = false;
                std::atomic<bool> done{false};
                std::atomic<bool> abandoned{false};
            };
            auto bctx = std::make_shared<BuildThreadContext>();
            bctx->engine = &engine;
            bctx->engineAlive = engine.getAliveToken();
            bctx->pluginVar = pv;
            juce::String gatePath = newPath;

            std::thread t([bctx, gatePath]() {
                bctx->engine->applyBackgroundThreadTuning();
#ifdef _WIN32
                ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
                std::unique_ptr<juce::AudioPluginInstance> tmpInst;
                juce::String tmpErr;
                bool tmpOk = false;
                if (!bctx->abandoned.load(std::memory_order_acquire) &&
                    (!bctx->engineAlive || bctx->engineAlive->load(std::memory_order_acquire))) {
                    tmpOk = bctx->engine->buildPluginFromVar(bctx->pluginVar, tmpInst, tmpErr) && (bool)tmpInst;
                }
                if (!bctx->abandoned.load(std::memory_order_acquire) &&
                    (!bctx->engineAlive || bctx->engineAlive->load(std::memory_order_acquire))) {
                    bctx->inst = std::move(tmpInst);
                    bctx->err = tmpErr;
                    bctx->ok = tmpOk;
                } else {
                    tmpInst = nullptr; // Destroy instance on worker BEFORE releasing gate
                }
#ifdef _WIN32
                ::CoUninitialize();
#endif
                // M13: ONLY the worker thread releases gatePath once it truly finishes.
                Fanfare::releasePathBuildGate(gatePath);
                bctx->done.store(true, std::memory_order_release);
            });

            int waitCount = 0;
            while (!bctx->done.load(std::memory_order_acquire) && waitCount < 600 &&
                   !(shouldAbort && shouldAbort())) {
                juce::Thread::sleep(100);
                waitCount++;
            }

            if (!bctx->done.load(std::memory_order_acquire)) {
                bctx->abandoned.store(true, std::memory_order_release);
                t.detach(); // Gate remains held until detached worker actually exits!
                if (shouldAbort && shouldAbort()) {
                    logToFile("TRACE: buildOne " + label + " aborted while building off-thread.");
                    return false;
                }
                Fanfare::poisonPath(gatePath);
                engine.markStagedBuildFailed(key);
                logToFile("WARNING: buildOne " + label + " off-thread build stalled (60s) — poisoning path and skipping concurrent retry while worker is still inside plugin.");
                r.ok = false;
                r.error = label + ": off-thread build stalled (60s)";
                r.failedEntries.push_back({key, pv, "off-thread build stalled (60s)"});
                return false;
            }

            t.join();
            inst = std::move(bctx->inst);
            err = bctx->err;
            ok = bctx->ok;

            if (!ok) {
                logToFile("TRACE: buildOne off-thread build failed for " + label + ". Error: " + err);
            }
        }

        if (shouldAbort && shouldAbort())
            return false;

        if (!ok) {
            if (!Fanfare::tryAcquirePathBuildGate(newPath, 120000, shouldAbort)) {
                if (shouldAbort && shouldAbort())
                    return false;
                r.ok = false;
                r.error = label + ": previous plugin build still wedged";
                r.failedEntries.push_back({key, pv, "previous build still wedged"});
                engine.markStagedBuildFailed(key);
                return false;
            }
            logToFile("TRACE: buildOne " + label + " building/retrying on message thread...");
            struct BuildContext {
                FanfareEngine* engine;
                std::shared_ptr<std::atomic<bool>> engineAlive;
                juce::var pluginVar;
                juce::String gatePath;
                std::unique_ptr<juce::AudioPluginInstance> inst;
                juce::String err;
                bool ok = false;
                juce::WaitableEvent done;
                std::atomic<bool> timedOut{false};
            };
            auto ctx = std::make_shared<BuildContext>();
            ctx->engine = &engine;
            ctx->engineAlive = engine.getAliveToken();
            ctx->pluginVar = pv;
            ctx->gatePath = newPath;

            juce::MessageManager::getInstance()->callAsync(
                [ctx]() {
                    std::unique_ptr<juce::AudioPluginInstance> tmpInst;
                    juce::String tmpErr;
                    bool tmpOk = false;
                    if (!ctx->timedOut.load(std::memory_order_acquire) &&
                        (!ctx->engineAlive || ctx->engineAlive->load(std::memory_order_acquire))) {
                        tmpOk = ctx->engine->buildPluginFromVar(ctx->pluginVar, tmpInst, tmpErr) &&
                                (bool)tmpInst;
                    }
                    if (!ctx->timedOut.load(std::memory_order_acquire)) {
                        ctx->inst = std::move(tmpInst);
                        ctx->err = tmpErr;
                        ctx->ok = tmpOk;
                    } else {
                        tmpInst.reset();
                    }
                    Fanfare::releasePathBuildGate(ctx->gatePath);
                    ctx->done.signal();
                });

            bool signaled = false;
            for (int w = 0; w < 300; ++w) {
                if (ctx->done.wait(200)) {
                    signaled = true;
                    break;
                }
                if (shouldAbort && shouldAbort())
                    break;
            }

            if (!signaled) {
                ctx->timedOut.store(true, std::memory_order_release);
                if (shouldAbort && shouldAbort()) {
                    logToFile("TRACE: buildOne aborted during message-thread wait: " + label);
                    return false;
                }
                Fanfare::poisonPath(newPath);
                engine.markStagedBuildFailed(key);
                logToFile("TRACE: buildOne timed out (60s) on message-thread build/retry: " + label);
                r.ok = false;
                r.error = label + ": build timed out (60s)";
                r.failedEntries.push_back({key, pv, "build timed out (60s)"});
                return false;
            }

            if (!ctx->ok) {
                engine.markStagedBuildFailed(key);
                logToFile("TRACE: buildOne message-thread build/retry failed for " + label + ": " + ctx->err);
                r.ok = false;
                r.error = label + ": " + ctx->err;
                const bool missingFile = !juce::File(pv.getProperty("path", "").toString()).exists();
                r.failedEntries.push_back({key, pv, missingFile ? juce::String("not installed") : ctx->err});
                return false;
            }
            inst = std::move(ctx->inst);
            ok = true;
        }

        logToFile("TRACE: buildOne staging " + label);
        if (isPreload)
            engine.pushPreloadedPlugin(key, std::move(inst));
        else
            engine.pushStagedPlugin(key, std::move(inst));
        ++r.builtCount;
        return true;
    }

    static bool buildMaster(FanfareEngine &engine, const juce::var &pv, bool isFoh,
                            int chainIdx, const juce::StringArray &masterPaths,
                            const juce::String &label, Result &r, bool isPreload = false,
                            std::function<bool()> shouldAbort = nullptr) {
        if (shouldAbort && shouldAbort())
            return false;
        juce::String key = FanfareEngine::stagingKeyFor(-1, chainIdx, isFoh);
        if (isPreload ? engine.hasPreloadedPlugin(key) : engine.stagingHasKey(key)) {
            logToFile("TRACE: buildMaster " + label + " already staged/preloaded, skipping build.");
            return true;
        }

        juce::String newPath = engine.normalizePath(pv.getProperty("path", "").toString());
        if (newPath.isEmpty())
            return true;

        juce::String curPath =
            (chainIdx < masterPaths.size()) ? masterPaths[chainIdx] : "";

        logToFile("TRACE: buildMaster " + label + " (Key: " + key + "). curPath: " + curPath + ", newPath: " + newPath);

        // Reuse-by-path for master FX plugins.
        if (curPath == newPath) {
            logToFile("TRACE: buildMaster " + label + " reusing existing plugin instance (curPath == newPath): " + newPath);
            ++r.reusedCount;
            return true;
        }

        if (Fanfare::isPathPoisoned(newPath)) {
            logToFile("WARNING: buildMaster " + label + " skipped — this plugin path already stalled past its timeout this session (" + newPath + ").");
            r.ok = false;
            r.error = label + ": skipped (plugin stalled earlier this session)";
            r.failedEntries.push_back({key, pv, "stalled earlier this session"});
            engine.markStagedBuildFailed(key);
            return false;
        }

        const bool requiresMessageThread = Fanfare::pluginRequiresMessageThread(newPath);

        std::unique_ptr<juce::AudioPluginInstance> inst;
        juce::String err;
        bool ok = false;

        if (!requiresMessageThread) {
            if (!Fanfare::tryAcquirePathBuildGate(newPath, 120000, shouldAbort)) {
                if (shouldAbort && shouldAbort())
                    return false;
                logToFile("WARNING: buildMaster " + label + " skipped — previous build of " + newPath + " is still wedged.");
                r.ok = false;
                r.error = label + ": previous plugin build still wedged";
                r.failedEntries.push_back({key, pv, "previous build still wedged"});
                engine.markStagedBuildFailed(key);
                return false;
            }
            logToFile("TRACE: buildMaster " + label + " building off-thread...");
            struct BuildThreadContext {
                FanfareEngine* engine;
                std::shared_ptr<std::atomic<bool>> engineAlive;
                juce::var pluginVar;
                std::unique_ptr<juce::AudioPluginInstance> inst;
                juce::String err;
                bool ok = false;
                std::atomic<bool> done{false};
                std::atomic<bool> abandoned{false};
            };
            auto bctx = std::make_shared<BuildThreadContext>();
            bctx->engine = &engine;
            bctx->engineAlive = engine.getAliveToken();
            bctx->pluginVar = pv;
            juce::String gatePath = newPath;

            std::thread t([bctx, gatePath]() {
                bctx->engine->applyBackgroundThreadTuning();
#ifdef _WIN32
                ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
                std::unique_ptr<juce::AudioPluginInstance> tmpInst;
                juce::String tmpErr;
                bool tmpOk = false;
                if (!bctx->abandoned.load(std::memory_order_acquire) &&
                    (!bctx->engineAlive || bctx->engineAlive->load(std::memory_order_acquire))) {
                    tmpOk = bctx->engine->buildPluginFromVar(bctx->pluginVar, tmpInst, tmpErr) && (bool)tmpInst;
                }
                if (!bctx->abandoned.load(std::memory_order_acquire) &&
                    (!bctx->engineAlive || bctx->engineAlive->load(std::memory_order_acquire))) {
                    bctx->inst = std::move(tmpInst);
                    bctx->err = tmpErr;
                    bctx->ok = tmpOk;
                } else {
                    tmpInst = nullptr;
                }
#ifdef _WIN32
                ::CoUninitialize();
#endif
                // M13: ONLY the worker thread releases gatePath once it truly finishes.
                Fanfare::releasePathBuildGate(gatePath);
                bctx->done.store(true, std::memory_order_release);
            });

            int waitCount = 0;
            while (!bctx->done.load(std::memory_order_acquire) && waitCount < 600 &&
                   !(shouldAbort && shouldAbort())) {
                juce::Thread::sleep(100);
                waitCount++;
            }

            if (!bctx->done.load(std::memory_order_acquire)) {
                bctx->abandoned.store(true, std::memory_order_release);
                t.detach(); // Gate remains held until detached worker actually exits!
                if (shouldAbort && shouldAbort()) {
                    logToFile("TRACE: buildMaster " + label + " aborted while building off-thread.");
                    return false;
                }
                Fanfare::poisonPath(gatePath);
                engine.markStagedBuildFailed(key);
                logToFile("WARNING: buildMaster " + label + " off-thread build stalled (60s) — poisoning path and skipping concurrent retry while worker is still inside plugin.");
                r.ok = false;
                r.error = label + ": off-thread build stalled (60s)";
                r.failedEntries.push_back({key, pv, "off-thread build stalled (60s)"});
                return false;
            }

            t.join();
            inst = std::move(bctx->inst);
            err = bctx->err;
            ok = bctx->ok;

            if (!ok) {
                logToFile("TRACE: buildMaster off-thread build failed for " + label + ". Error: " + err);
            }
        }

        if (shouldAbort && shouldAbort())
            return false;

        if (!ok) {
            if (!Fanfare::tryAcquirePathBuildGate(newPath, 120000, shouldAbort)) {
                if (shouldAbort && shouldAbort())
                    return false;
                r.ok = false;
                r.error = label + ": previous plugin build still wedged";
                r.failedEntries.push_back({key, pv, "previous build still wedged"});
                engine.markStagedBuildFailed(key);
                return false;
            }
            logToFile("TRACE: buildMaster " + label + " building/retrying on message thread...");
            struct BuildContext {
                FanfareEngine* engine;
                std::shared_ptr<std::atomic<bool>> engineAlive;
                juce::var pluginVar;
                juce::String gatePath;
                std::unique_ptr<juce::AudioPluginInstance> inst;
                juce::String err;
                bool ok = false;
                juce::WaitableEvent done;
                std::atomic<bool> timedOut{false};
            };
            auto ctx = std::make_shared<BuildContext>();
            ctx->engine = &engine;
            ctx->engineAlive = engine.getAliveToken();
            ctx->pluginVar = pv;
            ctx->gatePath = newPath;

            juce::MessageManager::getInstance()->callAsync(
                [ctx]() {
                    std::unique_ptr<juce::AudioPluginInstance> tmpInst;
                    juce::String tmpErr;
                    bool tmpOk = false;
                    if (!ctx->timedOut.load(std::memory_order_acquire) &&
                        (!ctx->engineAlive || ctx->engineAlive->load(std::memory_order_acquire))) {
                        tmpOk = ctx->engine->buildPluginFromVar(ctx->pluginVar, tmpInst, tmpErr) &&
                                (bool)tmpInst;
                    }
                    if (!ctx->timedOut.load(std::memory_order_acquire)) {
                        ctx->inst = std::move(tmpInst);
                        ctx->err = tmpErr;
                        ctx->ok = tmpOk;
                    } else {
                        tmpInst.reset();
                    }
                    Fanfare::releasePathBuildGate(ctx->gatePath);
                    ctx->done.signal();
                });

            bool signaled = false;
            for (int w = 0; w < 300; ++w) {
                if (ctx->done.wait(200)) {
                    signaled = true;
                    break;
                }
                if (shouldAbort && shouldAbort())
                    break;
            }

            if (!signaled) {
                ctx->timedOut.store(true, std::memory_order_release);
                if (shouldAbort && shouldAbort()) {
                    logToFile("TRACE: buildMaster aborted during message-thread wait: " + label);
                    return false;
                }
                Fanfare::poisonPath(newPath);
                engine.markStagedBuildFailed(key);
                logToFile("TRACE: buildMaster timed out (60s) on message-thread build/retry: " + label);
                r.ok = false;
                r.error = label + ": build timed out (60s)";
                r.failedEntries.push_back({key, pv, "build timed out (60s)"});
                return false;
            }

            if (!ctx->ok) {
                engine.markStagedBuildFailed(key);
                logToFile("TRACE: buildMaster message-thread build/retry failed for " + label + ": " + ctx->err);
                r.ok = false;
                r.error = label + ": " + ctx->err;
                r.failedEntries.push_back({key, pv, ctx->err.isEmpty() ? "load failed" : ctx->err});
                return false;
            }
            inst = std::move(ctx->inst);
            ok = true;
        }

        logToFile("TRACE: buildMaster staging " + label);
        if (isPreload)
            engine.pushPreloadedPlugin(key, std::move(inst));
        else
            engine.pushStagedPlugin(key, std::move(inst));
        ++r.builtCount;
        return true;
    }
};

} // namespace Fanfare
