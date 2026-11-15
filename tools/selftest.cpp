// tools/selftest.cpp - headless self-test harness for Fanfare.
// NOT part of the app build. Links against the app objects (minus Main.obj)
// and exercises the real load path without a GUI, audio device, or any
// power/affinity changes (those live in MainComponent, not the engine).
//
// Usage:
//   selftest <song.json> [--parse-only]   load one song, run all checks
//   selftest --web <port>                 start the tablet web server, idle
//
// Exit code: 0 = all checks passed, 1 = failures, 2 = usage error.

#include <JuceHeader.h>
#include "FanfareEngine.h"
#include "RigBuilder.h"
#include "FanfareConstants.h"
#include "PluginBuildGate.h"
#include "YoutubeDownloadManager.h"
#include "Web/OpenRigWebServer.h"
#include <atomic>
#include <cstdio>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: selftest <song.json> [--parse-only] | --web <port> | --unit\n");
        return 2;
    }

    // The MessageManager is thread-affine: create it on the pump thread and run
    // the dispatch loop there, so RigBuilder's callAsync build steps complete.
    std::atomic<bool> run{true};
    std::atomic<bool> ready{false};
    juce::MessageManager* mm = nullptr;
    std::thread pump([&] {
        juce::ScopedJuceInitialiser_GUI gui;
        mm = juce::MessageManager::getInstance();
        ready.store(true);
        mm->runDispatchLoop();
    });
    while (!ready.load())
        juce::Thread::sleep(5);

    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", msg);
        if (!cond)
            ++failures;
    };

    // Core security, RT hygiene, and build-gate unit checks
    check(YoutubeDownloadManager::isValidDownloadUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ"),
          "M1: valid https URL accepted");
    check(!YoutubeDownloadManager::isValidDownloadUrl("--exec calc.exe"),
          "M1: leading dash option-injection rejected");
    check(!YoutubeDownloadManager::isValidDownloadUrl("file:///C:/Windows/System32/calc.exe"),
          "M1: non-http scheme rejected");
    check(!YoutubeDownloadManager::isValidDownloadUrl("https://example.com/a b"),
          "M1: whitespace in URL rejected");
    check(Fanfare::pluginRequiresMessageThread("C:\\Program Files\\Common Files\\VST3\\Kontakt 8.vst3"),
          "L5: Kontakt 8 matches message-thread requirement");
    check(!Fanfare::pluginRequiresMessageThread("C:\\Users\\driver\\VST3\\CleanSynth.vst3"),
          "L5: parent directory named 'driver' does not false-positive");
    check(FanfareEngine::stagingKeyFor(-1, 0, true).isNotEmpty(),
          "H2: master FX staging key is non-empty");

    if (juce::String(argv[1]) == "--unit") {
        mm->stopDispatchLoop();
        pump.join();
        return failures ? 1 : 0;
    }

    if (juce::String(argv[1]) == "--web") {
        const int port = argc > 2 ? std::atoi(argv[2]) : 8080;
        FanfareEngine engine;
        OpenRig::WebServer server(engine, nullptr);
        const bool started = server.startServer(port);
        std::printf("web server on 127.0.0.1:%d started=%d\n", port, (int)started);
        check(started, "web server starts");
        while (run.load())
            juce::Thread::sleep(200);
        server.stopServer();
        mm->stopDispatchLoop();
        pump.join();
        return started ? 0 : 1;
    }

    const bool parseOnly = argc > 2 && juce::String(argv[2]) == "--parse-only";
    juce::File file(argv[1]);
    std::printf("== %s (%lld bytes)\n", file.getFileName().toRawUTF8(),
                (long long)file.getSize());
    std::fflush(stdout);
    if (!file.existsAsFile()) {
        std::printf("  [FAIL] file exists\n");
        mm->stopDispatchLoop();
        pump.join();
        return 1;
    }

    const auto text = file.loadFileAsString();
    const auto t0 = juce::Time::getCurrentTime();
    const auto parsed = juce::JSON::parse(text);
    const double parseMs = (juce::Time::getCurrentTime() - t0).inMilliseconds();
    check(parsed.isObject(), "song JSON parses to an object");
    std::printf("  parse: %.1f ms\n", parseMs);
    if (!parsed.isObject()) {
        mm->stopDispatchLoop();
        pump.join();
        return 1;
    }

    FanfareEngine engine;
    if (!parseOnly) {
        const auto t1 = juce::Time::getCurrentTime();
        auto res = Fanfare::RigBuilder::build(engine, parsed);
        const double buildMs = (juce::Time::getCurrentTime() - t1).inMilliseconds();
        std::printf("  build: ok=%d built=%d reused=%d failed=%d in %.0f ms\n",
                    (int)res.ok, res.builtCount, res.reusedCount,
                    (int)res.failedEntries.size(), buildMs);
        for (auto& fe : res.failedEntries)
            std::printf("    failed-entry: %s\n", fe.key.toRawUTF8());
        if (!res.error.isEmpty())
            std::printf("    error: %s\n", res.error.toRawUTF8());
        check(res.ok, "RigBuilder::build reports ok");

        int slotsWithPlugins = 0, totalChain = 0;
        for (int i = 0; i < FanfareConstants::kNumSlots; ++i) {
            auto* s = engine.getSlot(i);
            totalChain += s->getChainSize();
            if (s->hasActivePlugins())
                ++slotsWithPlugins;
        }
        std::printf("  live slots with plugins: %d, total chain entries: %d (built/staged: %d)\n",
                    slotsWithPlugins, totalChain, res.builtCount + res.reusedCount);
        // RigBuilder stages builds; they promote to live slots in the
        // transitioner, which the harness does not run - so assert on the
        // build result, not on live slots.
        check(res.builtCount + res.reusedCount > 0, "plugins were built/staged by RigBuilder");

        // Scene round-trip: change bypass on the keyboard slot, snapshot to
        // scene 0, undo, recall, and verify the state came back.
        engine.getSlot(1)->setBypass(true);
        engine.saveCurrentStateToScene(0);
        engine.getSlot(1)->setBypass(false);
        engine.loadScene(0);
        check(engine.getSlot(1)->isBypassed(), "scene save/recall restores slot bypass");

        // Export -> clear -> rebuild -> export must be byte-stable.
        const auto e1 = engine.exportRigToJson();
        engine.clearAllRacks();
        auto res2 = Fanfare::RigBuilder::build(engine, juce::JSON::parse(e1));
        const auto e2 = engine.exportRigToJson();
        std::printf("  round-trip: export1=%d bytes export2=%d bytes rebuilt=%d\n",
                    (int)e1.length(), (int)e2.length(), res2.builtCount + res2.reusedCount);
        check(e1 == e2, "export->rebuild->export is byte-stable");
        std::printf("  [INFO] rebuild after clear: built=%d reused=%d (staging cache may serve these)\n",
                    res2.builtCount, res2.reusedCount);
    }

    mm->stopDispatchLoop();
    pump.join();
    std::printf("== %s: %s (%d failures)\n", file.getFileName().toRawUTF8(),
                failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
