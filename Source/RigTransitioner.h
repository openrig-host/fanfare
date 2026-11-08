#pragma once

#include <JuceHeader.h>
#include "Logger.h"
#include "FanfareEngine.h"
#include "RigBuilder.h"
#include "RigSerializer.h"

namespace Fanfare {

/**
    RigTransitioner
    ---------------
    Orchestrates trustworthy song-to-song switching with rollback-by-
    construction: the live rack is NEVER touched until a fully validated
    replacement is ready.

    Pipeline (all off the audio thread; the message thread stays responsive):
      1. Load + migrate the rig JSON (RigSerializer).
      2. Build changed plugins off-thread into the staging cache, with silent
         processBlock validation (RigBuilder). Reuse-by-path = zero cost.
      3. If off-thread build failed, retry ONLY the failed plugins on the
         message thread (some VST3s require message-thread instantiation).
      4. Acquire the engine callback lock once and applyRig() (fast: heavy
         work already done, cached instances just swap in).
      5. Clear the staging cache and notify the UI.

    At no point is the current rig partially replaced: any failure before step
    4 leaves the live rack 100% intact.
*/
class RigTransitioner : public juce::Thread {
public:
    struct Callbacks {
        // Invoked on the message thread.
        std::function<void(juce::String)> onProgress;
        // Invoked on the message thread when the transition finishes.
        std::function<void(bool ok, juce::String message, int builtCount, const std::vector<RigBuilder::FailedEntry>& failedPlugins)> onComplete;
    };

    RigTransitioner(FanfareEngine &e)
        : juce::Thread("OpenRig Transitioner"), engine(e) {}

    ~RigTransitioner() override {
        aliveFlag->store(false, std::memory_order_release);
        stopTransition();
    }

    bool isBusy() const { return isThreadRunning(); }

    /** Begin an async transition to a rig file. Call from the message thread. */
    void transitionToFile(const juce::File &file, Callbacks cbs) {
        jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
        if (isThreadRunning())
            return;
        auto st = std::make_shared<RunState>();
        st->engine = &engine;
        st->engineAlive = engine.getAliveToken();
        st->alive = aliveFlag;
        st->useFile = true;
        st->sourceFile = file;
        st->callbacks = std::move(cbs);
        {
            juce::ScopedLock sl(stateLock);
            currentRun = st;
        }
        startThread();
    }

    /** Begin an async transition from a JSON string. Call from the message thread. */
    void transitionToJson(const juce::String &json, Callbacks cbs) {
        jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
        if (isThreadRunning())
            return;
        auto st = std::make_shared<RunState>();
        st->engine = &engine;
        st->engineAlive = engine.getAliveToken();
        st->alive = aliveFlag;
        st->useFile = false;
        st->sourceJson = json;
        st->callbacks = std::move(cbs);
        {
            juce::ScopedLock sl(stateLock);
            currentRun = st;
        }
        startThread();
    }

    void stopTransition() {
        if (isThreadRunning()) {
            signalThreadShouldExit();
            waitForThreadToExit(-1);
        }
        if (aliveFlag->load(std::memory_order_acquire))
            engine.clearStagingCache();
    }

    void run() override {
        std::shared_ptr<RunState> st;
        {
            juce::ScopedLock sl(stateLock);
            st = currentRun;
        }
        if (!st || !st->engine || (st->engineAlive && !st->engineAlive->load(std::memory_order_acquire)))
            return;

        FanfareEngine &eng = *st->engine;
        logToFile("TRACE: RigTransitioner run() started.");
        if (eng.hasPreloadedPlugins()) {
            logToFile("TRACE: Promoting preloaded plugins to staging cache.");
            eng.promotePreloadedToStaged();
        } else {
            eng.clearStagingCache();
        }

        // 1. Load + migrate.
        postProgress(st, "Loading rig...");
        logToFile("TRACE: RigTransitioner loading setup data.");
        RigSerializer::LoadResult loaded =
            st->useFile ? RigSerializer::load(st->sourceFile)
                        : RigSerializer::loadFromString(st->sourceJson);

        if (!loaded.ok) {
            logToFile("TRACE: RigTransitioner load failed: " + loaded.errorMessage);
            postComplete(st, false, loaded.errorMessage, 0);
            return;
        }

        // Clean Slate (freshBuild): If requested by the setup, completely unload
        // all existing plugins from the live rack first so that complex setups
        // (multi-Omnisphere, Kontakt + Zenology, heavy VSTs) build with full isolation.
        bool isFreshBuild = (bool)loaded.rig.getProperty("freshBuild", false);
        if (isFreshBuild) {
            logToFile("TRACE: RigTransitioner freshBuild=true detected. Unloading all current plugins before build.");
            postProgress(st, "Unloading previous rig (Clean Slate)...");
            eng.clearStagingCache();
            eng.clearPreloadedCache();

            auto clearDone = std::make_shared<juce::WaitableEvent>();
            juce::MessageManager::getInstance()->callAsync([st, clearDone]() {
                if (st->alive->load(std::memory_order_acquire) &&
                    (!st->engineAlive || st->engineAlive->load(std::memory_order_acquire)))
                    st->engine->clearAllRacks();
                clearDone->signal();
            });
            bool cleared = false;
            for (int w = 0; w < 150; ++w) {
                if (clearDone->wait(200)) { cleared = true; break; }
                if (threadShouldExit() || !st->alive->load(std::memory_order_acquire)) break;
            }
            if (!cleared && !threadShouldExit()) {
                logToFile("WARNING: clearAllRacks timed out (30s) waiting on message thread.");
            }
        }

        if (threadShouldExit() || !st->alive->load(std::memory_order_acquire)) {
            logToFile("TRACE: RigTransitioner exit requested after clear.");
            postComplete(st, false, "Cancelled", 0, {});
            return;
        }

        // 2. Build on this worker thread (keeps UI responsive; heavy plugins
        //    like Kontakt won't freeze the app). Reuse-by-path ensures we
        //    never create a second instance of the same plugin. Exception
        //    guards in buildPluginFromVar catch any plugin that throws.
        postProgress(st, "Building plugins...");
        RigBuilder::Result build = RigBuilder::build(
            eng, loaded.rig,
            [st](const juce::String &msg) { postProgress(st, msg); },
            false,
            [this, st]() { return threadShouldExit() || !st->alive->load(std::memory_order_acquire); });
        logToFile("TRACE: RigBuilder finished. ok=" + juce::String((int)build.ok) +
                  ", built=" + juce::String(build.builtCount) +
                  ", reused=" + juce::String(build.reusedCount) +
                  ", failed=" + juce::String((int)build.failedEntries.size()));

        // 3. If some plugins failed, log the error but CONTINUE — the staging
        //    cache has the successful ones, and failed slots stay empty.
        //    We do NOT retry on the message thread: heavy plugins like Kontakt
        //    can hang the message thread for minutes, freezing the UI.
        if (!build.ok) {
            logToFile("WARNING: " + juce::String((int)build.failedEntries.size()) +
                      " plugin(s) failed to build: " + build.error);
        }

        if (threadShouldExit() || !st->alive->load(std::memory_order_acquire)) {
            logToFile("TRACE: RigTransitioner exit requested before apply.");
            postComplete(st, false, "Cancelled", 0, {});
            return;
        }

        // 4. Atomic apply under the callback lock on the message thread.
        //    Timeout prevents hanging if a plugin blocks during state restore.
        postProgress(st, "Applying...");
        logToFile("TRACE: RigTransitioner dispatching applyRig to main thread.");
        auto sharedRig = std::make_shared<juce::var>(std::move(loaded.rig));
        auto doneEvent = std::make_shared<juce::WaitableEvent>();
        juce::MessageManager::getInstance()->callAsync([st, sharedRig, doneEvent]() {
            if (st->alive->load(std::memory_order_acquire) &&
                (!st->engineAlive || st->engineAlive->load(std::memory_order_acquire))) {
                logToFile("TRACE: RigTransitioner applying rig on main thread.");
                st->engine->applyRig(*sharedRig);
            }
            doneEvent->signal();
        });
        bool applied = false;
        for (int w = 0; w < 600; ++w) {
            if (doneEvent->wait(200)) { applied = true; break; }
            if (threadShouldExit() || !st->alive->load(std::memory_order_acquire)) break;
        }
        if (!applied) {
            if (threadShouldExit() || !st->alive->load(std::memory_order_acquire))
                return;
            logToFile("ERROR: applyRig timed out (120s)");
            eng.clearStagingCache();
            postComplete(st, false, "Apply timed out (120s) - possible plugin hang", 0, {});
            return;
        }
        logToFile("TRACE: RigTransitioner applyRig finished.");

        // 5. Clean up and notify.
        eng.clearStagingCache();
        int total = build.builtCount + build.reusedCount;
        juce::String msg = "Loaded (" + juce::String(build.builtCount) + " built, " +
                           juce::String(build.reusedCount) + " reused)";
        logToFile("TRACE: RigTransitioner completed successfully: " + msg);
        postComplete(st, true, msg, total, build.failedEntries);
    }

private:
    struct RunState {
        FanfareEngine *engine = nullptr;
        std::shared_ptr<std::atomic<bool>> engineAlive;
        std::shared_ptr<std::atomic<bool>> alive;
        bool useFile = true;
        juce::File sourceFile;
        juce::String sourceJson;
        Callbacks callbacks;
    };

    static void postProgress(const std::shared_ptr<RunState> &st, const juce::String &msg) {
        if (st && st->callbacks.onProgress && st->alive->load(std::memory_order_acquire)) {
            auto cb = st->callbacks.onProgress;
            auto alive = st->alive;
            juce::MessageManager::callAsync([alive, cb, msg]() {
                if (alive->load(std::memory_order_acquire))
                    cb(msg);
            });
        }
    }

    static void postComplete(const std::shared_ptr<RunState> &st, bool ok, const juce::String &msg,
                             int builtCount, const std::vector<RigBuilder::FailedEntry> &failed = {}) {
        if (st && st->callbacks.onComplete && st->alive->load(std::memory_order_acquire)) {
            auto cb = st->callbacks.onComplete;
            auto alive = st->alive;
            juce::MessageManager::callAsync([alive, cb, ok, msg, builtCount, failed]() {
                if (alive->load(std::memory_order_acquire))
                    cb(ok, msg, builtCount, failed);
            });
        }
    }

    std::shared_ptr<std::atomic<bool>> aliveFlag = std::make_shared<std::atomic<bool>>(true);
    FanfareEngine &engine;
    juce::CriticalSection stateLock;
    std::shared_ptr<RunState> currentRun;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RigTransitioner)
};

} // namespace Fanfare
