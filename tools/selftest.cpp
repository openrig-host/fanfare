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
#include "AppSettings.h"
#include "CpuTopology.h"
#include "FanfareEngine.h"
#include "GlitchTelemetry.h"
#include "HelpLauncher.h"
#include "RigBuilder.h"
#include "FanfareConstants.h"
#include "PluginBuildGate.h"
#include "StripState.h"
#include "YoutubeDownloadManager.h"
#include "Web/OpenRigWebServer.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <thread>

namespace {

template <typename CheckFn>
void runDefectRegressionTests(CheckFn&& check) {
    // Defect 1: compMakeupDb & compPreset persist through rig, scene, strip, and RigSerializer
    {
        FanfareEngine eng;
        auto* s0 = eng.getSlot(0);
        s0->getStrip().compEnabled.store(true);
        s0->getStrip().compAmount.store(0.65f);
        s0->getStrip().compMakeupDb.store(7.5f);
        s0->getStrip().compPreset.store(3);

        const auto exportedJson = eng.exportRigToJson();

        // Overwrite with distinct values, then re-import exported JSON
        s0->getStrip().compEnabled.store(false);
        s0->getStrip().compAmount.store(0.0f);
        s0->getStrip().compMakeupDb.store(-5.0f);
        s0->getStrip().compPreset.store(0);
        eng.importRigFromJson(exportedJson);
        check(s0->getStrip().compEnabled.load() &&
              std::abs(s0->getStrip().compAmount.load() - 0.65f) < 1e-4f &&
              std::abs(s0->getStrip().compMakeupDb.load() - 7.5f) < 1e-4f &&
              s0->getStrip().compPreset.load() == 3,
              "D1: rig export/import preserves compMakeupDb and compPreset");

        // Scene snapshot/recall round-trip
        eng.saveCurrentStateToScene(0);
        s0->getStrip().compMakeupDb.store(-3.0f);
        s0->getStrip().compPreset.store(1);
        eng.loadScene(0);
        check(std::abs(s0->getStrip().compMakeupDb.load() - 7.5f) < 1e-4f &&
              s0->getStrip().compPreset.load() == 3,
              "D1: scene save/load restores compMakeupDb and compPreset");

        // Strip file round-trip
        const auto exportedSlot = eng.exportSlot(0);
        const auto stripJson = Fanfare::RigSerializer::serializeStrip(exportedSlot);
        const auto tempStrip = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getChildFile("fanfare_selftest_defect1_strip.json");
        tempStrip.deleteFile();
        tempStrip.replaceWithText(stripJson);
        Fanfare::SongSlot loadedSlot;
        const bool readStripOk = Fanfare::RigSerializer::readStripFromFile(tempStrip, loadedSlot);
        tempStrip.deleteFile();
        eng.importSlot(1, loadedSlot);
        check(readStripOk &&
              std::abs(eng.getSlot(1)->getStrip().compMakeupDb.load() - 7.5f) < 1e-4f &&
              eng.getSlot(1)->getStrip().compPreset.load() == 3,
              "D1: strip serialize/read/import preserves compMakeupDb and compPreset");

        // RigSerializer Song model round-trip and legacy default fallback
        Fanfare::Song parsedSong;
        const bool varOk = Fanfare::RigSerializer::varToSong(juce::JSON::parse(exportedJson), parsedSong);
        Fanfare::Song reparsedSong;
        const bool serOk = varOk && Fanfare::RigSerializer::varToSong(
            juce::JSON::parse(Fanfare::RigSerializer::serializeSong(parsedSong)), reparsedSong);
        check(serOk && !reparsedSong.slots.empty() &&
              std::abs(reparsedSong.slots[0].strip.compMakeupDb - 7.5f) < 1e-4f &&
              reparsedSong.slots[0].strip.compPreset == 3,
              "D1: RigSerializer preserves compMakeupDb and compPreset");

        auto legacyVar = juce::JSON::parse(exportedJson);
        if (auto* chans = legacyVar.getProperty("channels", juce::var()).getArray()) {
            if (!chans->isEmpty()) {
                if (auto* stripObj = chans->getReference(0).getProperty("strip", juce::var()).getDynamicObject()) {
                    stripObj->removeProperty("compMakeupDb");
                    stripObj->removeProperty("compPreset");
                }
            }
        }
        if (auto* scs = legacyVar.getProperty("scenes", juce::var()).getArray()) {
            if (!scs->isEmpty()) {
                if (auto* sts = scs->getReference(0).getProperty("states", juce::var()).getArray()) {
                    if (!sts->isEmpty()) {
                        if (auto* stObj = sts->getReference(0).getDynamicObject()) {
                            stObj->removeProperty("compMakeupDb");
                            stObj->removeProperty("compPreset");
                        }
                    }
                }
            }
        }
        Fanfare::Song legacySong;
        const bool legacyOk = Fanfare::RigSerializer::varToSong(legacyVar, legacySong);
        check(legacyOk && !legacySong.slots.empty() &&
              std::abs(legacySong.slots[0].strip.compMakeupDb - 0.0f) < 1e-4f &&
              legacySong.slots[0].strip.compPreset == 0 &&
              !legacySong.scenes.empty() && !legacySong.scenes[0].slotStates.empty() &&
              std::abs(legacySong.scenes[0].slotStates[0].strip.compMakeupDb - 0.0f) < 1e-4f &&
              legacySong.scenes[0].slotStates[0].strip.compPreset == 0,
              "D1: missing compMakeupDb/compPreset default safely to 0");
    }

    // Defect 2: per-layer MIDI channel persists across rig, clear/reload, and strip files
    {
        FanfareEngine eng;
        auto* s0 = eng.getSlot(0);
        s0->setChainSlotSynth(0, true);
        s0->getChainSlotSettings(0).midiChannel.store(7);
        s0->setChainSlotFm(1, true);
        s0->getChainSlotSettings(1).midiChannel.store(12);

        const auto rigJson = eng.exportRigToJson();
        s0->getChainSlotSettings(0).midiChannel.store(0);
        s0->getChainSlotSettings(1).midiChannel.store(0);
        eng.clearAllRacks();
        eng.importRigFromJson(rigJson);
        check(s0->getChainSlotSettings(0).midiChannel.load() == 7 &&
              s0->getChainSlotSettings(1).midiChannel.load() == 12,
              "D2: rig export/clear/import preserves per-layer midiChannel");

        const auto slotModel = eng.exportSlot(0);
        const auto stripJson = Fanfare::RigSerializer::serializeStrip(slotModel);
        const auto tempStrip = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getChildFile("fanfare_selftest_defect2_strip.json");
        tempStrip.deleteFile();
        tempStrip.replaceWithText(stripJson);
        Fanfare::SongSlot loadedStrip;
        const bool readOk = Fanfare::RigSerializer::readStripFromFile(tempStrip, loadedStrip);
        tempStrip.deleteFile();
        eng.importSlot(2, loadedStrip);
        check(readOk &&
              eng.getSlot(2)->getChainSlotSettings(0).midiChannel.load() == 7 &&
              eng.getSlot(2)->getChainSlotSettings(1).midiChannel.load() == 12,
              "D2: strip serialize/read/import preserves per-layer midiChannel");

        auto noChVar = juce::JSON::parse(rigJson);
        if (auto* chans = noChVar.getProperty("channels", juce::var()).getArray()) {
            if (!chans->isEmpty()) {
                if (auto* chArr = chans->getReference(0).getProperty("chain", juce::var()).getArray()) {
                    for (int p = 0; p < chArr->size(); ++p) {
                        if (auto* po = chArr->getReference(p).getDynamicObject())
                            po->removeProperty("midiChannel");
                    }
                }
            }
        }
        eng.applyRig(noChVar);
        check(s0->getChainSlotSettings(0).midiChannel.load() == 0 &&
              s0->getChainSlotSettings(1).midiChannel.load() == 0,
              "D2: loading JSON without layer midiChannel resets layer channel to 0 (Strip)");
    }

    // Defect 3: arpeggiator defaults agree across live processor, scene SlotState, and SongSlot::ArpParams
    {
        SimpleArpeggiator arp;
        SlotState st;
        Fanfare::SongSlot::ArpParams ap;
        check(std::abs(arp.bpm.load() - FanfareConstants::kDefaultArpBpm) < 1e-4f &&
              arp.octavesUp.load() == FanfareConstants::kDefaultArpOctavesUp &&
              arp.octavesDown.load() == FanfareConstants::kDefaultArpOctavesDown &&
              std::abs(arp.gate.load() - FanfareConstants::kDefaultArpGate) < 1e-4f &&
              arp.patternIdx.load() == FanfareConstants::kDefaultArpPatternIdx &&
              std::abs(st.arp.bpm - FanfareConstants::kDefaultArpBpm) < 1e-4f &&
              st.arp.octavesUp == FanfareConstants::kDefaultArpOctavesUp &&
              st.arp.octavesDown == FanfareConstants::kDefaultArpOctavesDown &&
              std::abs(st.arp.gate - FanfareConstants::kDefaultArpGate) < 1e-4f &&
              st.arp.patternIdx == FanfareConstants::kDefaultArpPatternIdx &&
              std::abs(ap.bpm - FanfareConstants::kDefaultArpBpm) < 1e-4f &&
              ap.octavesUp == FanfareConstants::kDefaultArpOctavesUp &&
              ap.octavesDown == FanfareConstants::kDefaultArpOctavesDown &&
              std::abs(ap.gate - FanfareConstants::kDefaultArpGate) < 1e-4f &&
              ap.patternIdx == FanfareConstants::kDefaultArpPatternIdx,
              "D3: arpeggiator defaults match FanfareConstants::kDefaultArp* everywhere");
    }

    // Defect 4: MP3 ListenerList supports auto-unbypass and external listeners simultaneously
    {
        struct CountingMp3Listener : Mp3PlayerProcessor::Listener {
            int playStartedCount = 0;
            void mp3PlayStarted() override { ++playStartedCount; }
        };

        const auto tempWav = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                 .getChildFile("fanfare_selftest_mp3_listener.wav");
        tempWav.deleteFile();
        bool wavWritten = false;
        {
            juce::WavAudioFormat wavFormat;
            std::unique_ptr<juce::FileOutputStream> fos(tempWav.createOutputStream());
            if (fos && fos->openedOk()) {
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    wavFormat.createWriterFor(fos.get(), 44100.0, 2, 16, {}, 0));
                if (writer) {
                    fos.release();
                    juce::AudioBuffer<float> silence(2, 4410);
                    silence.clear();
                    wavWritten = writer->writeFromAudioSampleBuffer(silence, 0, silence.getNumSamples());
                }
            }
        }

        RackSlot mp3Slot("TestMp3");
        mp3Slot.setBypass(true);
        mp3Slot.getMp3Player().prepare(44100.0, 512);
        CountingMp3Listener listener;
        mp3Slot.getMp3Player().addListener(&listener);

        const bool loaded = wavWritten && mp3Slot.getMp3Player().loadFileToDeck(0, tempWav, false);
        mp3Slot.getMp3Player().play();

        check(loaded && !mp3Slot.isBypassed() && listener.playStartedCount >= 1,
              "D4: MP3 play notifies both RackSlot auto-unbypass and external listener");

        mp3Slot.getMp3Player().stop();
        mp3Slot.getMp3Player().removeListener(&listener);
        tempWav.deleteFile();
    }
}

template <typename CheckFn>
void runOfflineRenderTest(CheckFn&& check) {
    FanfareEngine engine;
    constexpr int kBlockSize = 512;
    constexpr double kSampleRate = 44100.0;
    engine.prepareToPlay(kBlockSize, kSampleRate);

    auto* s0 = engine.getSlot(0);
    s0->setChainSlotSynth(0, true);
    s0->getChainSlotSettings(0).enabled.store(true);
    s0->setBypass(false);
    s0->setFohEnabled(true);
    s0->setChannelLevel(0.8f);
    s0->setMidiChannelOverride(0);
    s0->getStrip().compEnabled.store(true);
    s0->getStrip().compAmount.store(0.5f);
    s0->getStrip().compMakeupDb.store(3.0f);
    s0->getStrip().compPreset.store(2);
    s0->getStrip().eqEnabled.store(true);

    juce::AudioBuffer<float> inBuf(2, kBlockSize);
    inBuf.clear();
    juce::AudioBuffer<float> outBuf(4, kBlockSize);
    const float* inPtrs[2] = {inBuf.getReadPointer(0), inBuf.getReadPointer(1)};
    float* outPtrs[4] = {
        outBuf.getWritePointer(0), outBuf.getWritePointer(1),
        outBuf.getWritePointer(2), outBuf.getWritePointer(3)
    };

    float maxPeak = 0.0f;
    bool allFinite = true;
    for (int blk = 0; blk < 10; ++blk) {
        juce::MidiBuffer midi;
        if (blk == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
        if (blk == 6)
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);

        outBuf.clear();
        engine.processAudio(inPtrs, 2, outPtrs, 4, kBlockSize, midi);

        for (int ch = 0; ch < outBuf.getNumChannels(); ++ch) {
            const float* rp = outBuf.getReadPointer(ch);
            for (int n = 0; n < kBlockSize; ++n) {
                const float v = rp[n];
                if (!std::isfinite(v))
                    allFinite = false;
                else
                    maxPeak = juce::jmax(maxPeak, std::abs(v));
            }
        }
    }

    const bool underrun = engine.consumeAudioUnderrun();
    std::printf("  offline-render active peak=%.6f finite=%d underrun=%d\n",
                maxPeak, (int)allFinite, (int)underrun);
    check(allFinite && maxPeak > 1e-4f && maxPeak < 10.0f && !underrun,
          "OfflineRender: built-in synth produces finite bounded non-silent audio");

    s0->setBypass(true);
    for (int blk = 0; blk < 20; ++blk) {
        juce::MidiBuffer emptyMidi;
        outBuf.clear();
        engine.processAudio(inPtrs, 2, outPtrs, 4, kBlockSize, emptyMidi);
    }

    float bypassPeak = 0.0f;
    bool bypassFinite = true;
    {
        juce::MidiBuffer emptyMidi;
        outBuf.clear();
        engine.processAudio(inPtrs, 2, outPtrs, 4, kBlockSize, emptyMidi);
        for (int ch = 0; ch < outBuf.getNumChannels(); ++ch) {
            const float* rp = outBuf.getReadPointer(ch);
            for (int n = 0; n < kBlockSize; ++n) {
                const float v = rp[n];
                if (!std::isfinite(v))
                    bypassFinite = false;
                else
                    bypassPeak = juce::jmax(bypassPeak, std::abs(v));
            }
        }
    }
    std::printf("  offline-render bypassed peak=%.6f finite=%d\n",
                bypassPeak, (int)bypassFinite);
    check(bypassFinite && bypassPeak < 1e-6f,
          "OfflineRender: bypassed slot renders silence");
}

template <typename CheckFn>
void runPhase2Tests(CheckFn&& check) {
    // Item 1: StripState canonical JSON codecs + legacy scene musette/channelMode handling
    {
        Fanfare::ChannelStripSettings src;
        src.gateEnabled = true;
        src.gateThreshold = -42.5f;
        src.eqEnabled = true;
        src.eqCurveMode = true;
        src.hpfFreq = 85.0f;
        src.lowShelfGain = 2.5f;
        src.highShelfGain = -1.5f;
        src.eqVolGain = 1.0f;
        src.eqMasterGain = -0.5f;
        src.eqBands[3] = 4.25f;
        src.compEnabled = true;
        src.compAmount = 0.72f;
        src.compMakeupDb = 5.5f;
        src.compPreset = 2;
        src.chorusEnabled = true;
        src.chorusRate = 2.3f;
        src.chorusMix = 0.45f;
        src.delayEnabled = true;
        src.delayMode = 3;
        src.delayTimeMs = 420.0f;
        src.delayFeedback = 0.48f;
        src.delayTone = 4800.0f;
        src.delayMix = 0.31f;
        src.reverbEnabled = true;
        src.reverbSize = 0.77f;
        src.reverbMix = 0.28f;
        src.irEnabled = true;
        src.irMix = 0.4f;
        src.irPath = "Hall";
        src.musetteEnabled = true;
        src.musetteCents = 14.0f;
        src.musetteMode = 2;
        src.musetteMix = 0.33f;
        src.channelMode = 2;

        juce::DynamicObject::Ptr stripObj = new juce::DynamicObject();
        Fanfare::writeStripToObject(*stripObj, src, true);
        Fanfare::ChannelStripSettings dst;
        Fanfare::readStripFromObject(juce::var(stripObj.get()), dst, true);
        check(dst.gateEnabled == src.gateEnabled &&
              std::abs(dst.gateThreshold - src.gateThreshold) < 1e-4f &&
              dst.eqCurveMode == src.eqCurveMode &&
              std::abs(dst.eqBands[3] - src.eqBands[3]) < 1e-4f &&
              std::abs(dst.compMakeupDb - src.compMakeupDb) < 1e-4f &&
              dst.compPreset == src.compPreset &&
              dst.delayMode == src.delayMode &&
              dst.irPath == src.irPath &&
              dst.musetteEnabled == src.musetteEnabled &&
              dst.musetteMode == src.musetteMode &&
              dst.channelMode == src.channelMode,
              "Item1: writeStripToObject/readStripFromObject round-trips all fields");

        Fanfare::ArpParams arpSrc{true, 134.0f, 2, 1, 0.65f, 2};
        Fanfare::HarmParams harmSrc{true, 2, 1, 1, 5};
        for (bool scenePrefixed : {false, true}) {
            juce::DynamicObject::Ptr obj = new juce::DynamicObject();
            Fanfare::writeArpToObject(*obj, arpSrc, scenePrefixed);
            Fanfare::writeHarmToObject(*obj, harmSrc, scenePrefixed);
            Fanfare::ArpParams arpDst;
            Fanfare::HarmParams harmDst;
            Fanfare::readArpFromObject(juce::var(obj.get()), arpDst, scenePrefixed);
            Fanfare::readHarmFromObject(juce::var(obj.get()), harmDst, scenePrefixed);
            check(arpDst.enabled == arpSrc.enabled &&
                  std::abs(arpDst.bpm - arpSrc.bpm) < 1e-4f &&
                  arpDst.octavesUp == arpSrc.octavesUp &&
                  arpDst.octavesDown == arpSrc.octavesDown &&
                  std::abs(arpDst.gate - arpSrc.gate) < 1e-4f &&
                  arpDst.patternIdx == arpSrc.patternIdx &&
                  harmDst.enabled == harmSrc.enabled &&
                  harmDst.octavesUp == harmSrc.octavesUp &&
                  harmDst.octavesDown == harmSrc.octavesDown &&
                  harmDst.africaMode == harmSrc.africaMode &&
                  harmDst.harmonyTargetSlot == harmSrc.harmonyTargetSlot,
                  scenePrefixed ? "Item1: scene-prefixed Arp/Harm JSON round-trip OK"
                                : "Item1: song-nested Arp/Harm JSON round-trip OK");
        }

        // Legacy scene JSON without musette/channelMode must NOT clobber live musette/channelMode;
        // newly captured scenes DO snapshot and restore musette/channelMode.
        FanfareEngine eng;
        auto* s0 = eng.getSlot(0);
        const auto baseJson = eng.exportRigToJson();
        auto legacyRig = juce::JSON::parse(baseJson);
        if (auto* scs = legacyRig.getProperty("scenes", juce::var()).getArray()) {
            if (!scs->isEmpty()) {
                if (auto* sts = scs->getReference(0).getProperty("states", juce::var()).getArray()) {
                    if (!sts->isEmpty()) {
                        if (auto* stObj = sts->getReference(0).getDynamicObject()) {
                            stObj->removeProperty("musetteEnabled");
                            stObj->removeProperty("musetteCents");
                            stObj->removeProperty("musette3Reed");
                            stObj->removeProperty("musetteMode");
                            stObj->removeProperty("musetteMix");
                            stObj->removeProperty("channelMode");
                        }
                    }
                }
            }
        }
        eng.applyRig(legacyRig);
        s0->getStrip().musetteEnabled.store(true);
        s0->getStrip().channelMode.store(2);
        eng.loadScene(0);
        check(s0->getStrip().musetteEnabled.load() == true &&
              s0->getStrip().channelMode.load() == 2,
              "Item1: legacy scene without musette/channelMode preserves live values");

        eng.saveCurrentStateToScene(0);
        s0->getStrip().musetteEnabled.store(false);
        s0->getStrip().channelMode.store(0);
        eng.loadScene(0);
        check(s0->getStrip().musetteEnabled.load() == true &&
              s0->getStrip().channelMode.load() == 2,
              "Item1: newly captured scene snapshots and restores musette/channelMode");
    }

    // Item 6: AppSettings isolated persistence, .bak creation, and corrupt-file guard
    {
        const auto tmpFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                 .getChildFile("fanfare_selftest_appsettings.json");
        const auto bakFile = tmpFile.getSiblingFile(tmpFile.getFileName() + ".bak");
        tmpFile.deleteFile();
        bakFile.deleteFile();

        {
            Fanfare::AppSettings s1(tmpFile);
            check(s1.setBool("flagA", true) &&
                  s1.setInt("portNum", 8123) &&
                  s1.setString("bindScope", "lan") &&
                  s1.update([](juce::DynamicObject& o) { o.setProperty("extra", "ok"); }),
                  "Item6: AppSettings writes typed values atomically");
            check(bakFile.existsAsFile(), "Item6: AppSettings writes .bak backup on update");

            Fanfare::AppSettings s2(tmpFile);
            check(s2.getBool("flagA", false) == true &&
                  s2.getInt("portNum", 0) == 8123 &&
                  s2.getString("bindScope") == "lan" &&
                  s2.getString("extra") == "ok",
                  "Item6: fresh AppSettings instance reloads persisted values");
        }

        const juce::String corruptRaw = "{not valid json";
        tmpFile.replaceWithText(corruptRaw);
        {
            Fanfare::AppSettings sCorrupt(tmpFile);
            const bool writeOk = sCorrupt.setBool("flagA", false);
            check(!writeOk && tmpFile.loadFileAsString() == corruptRaw,
                  "Item6: AppSettings refuses to clobber corrupt non-empty JSON");
        }
        tmpFile.deleteFile();
        bakFile.deleteFile();
    }

    // Item 7: WebServer bind scope (Off / Loopback), auth gating, and offline manual extraction
    {
        FanfareEngine eng;
        OpenRig::WebServer server(eng, nullptr);

        check(server.startServer(0, OpenRig::WebServer::BindScope::Off) && !server.isRunning(),
              "Item7: BindScope::Off keeps server stopped");

        const int testPort = 18924;
        const bool startedLoopback =
            server.startServer(testPort, OpenRig::WebServer::BindScope::Loopback);
        check(startedLoopback && server.isRunning() &&
              server.getBindScope() == OpenRig::WebServer::BindScope::Loopback,
              "Item7: BindScope::Loopback starts server cleanly");

        int authStatus = 0;
        juce::String authBody;
        if (auto stream = juce::URL("http://127.0.0.1:" + juce::String(testPort) +
                                    "/api/status?token=" + server.getSessionToken())
                              .createInputStream(juce::URL::InputStreamOptions(
                                                     juce::URL::ParameterHandling::inAddress)
                                                     .withConnectionTimeoutMs(2000)
                                                     .withStatusCode(&authStatus))) {
            authBody = stream->readEntireStreamAsString();
        }
        const auto parsedStatus = juce::JSON::parse(authBody);
        check(authStatus == 200 && parsedStatus.isObject() &&
              (bool)parsedStatus.getProperty("ok", false),
              "Item7: authenticated GET /api/status returns 200 JSON over loopback");

        int unauthStatus = 0;
        (void)juce::URL("http://127.0.0.1:" + juce::String(testPort) + "/api/status")
            .createInputStream(juce::URL::InputStreamOptions(
                                   juce::URL::ParameterHandling::inAddress)
                                   .withConnectionTimeoutMs(2000)
                                   .withStatusCode(&unauthStatus));
        check(unauthStatus == 401, "Item7: unauthenticated GET /api/status returns 401");

        server.stopServer();
        check(!server.isRunning(), "Item7: WebServer stopServer shuts down cleanly");

        const juce::File manualFile = Fanfare::ensureOfflineManualExtracted();
        const juce::File cssFile = manualFile.getParentDirectory()
                                       .getChildFile("assets-readonly")
                                       .getChildFile("css")
                                       .getChildFile("main.css");
        check(manualFile.existsAsFile() && manualFile.getSize() > 1000 &&
              cssFile.existsAsFile(),
              "Item7: offline manual extracts without running web server");
    }
}

template <typename CheckFn>
void runPhase3Tests(CheckFn&& check) {
    // Items 2 & 7: IWebHost decoupling, dispatchApiPost table, and plugin mirror unregistration
    {
        struct MockWebHost final : public OpenRig::IWebHost {
            int lastRecalledScene = -1;
            juce::File lastLoadedRig;
            int lastLoadedSetlistIdx = -1;
            double cpuUsage = 17.5;
            int underruns = 3;
            bool thermalWarn = false;
            int cpuTempC = 64;
            juce::String thermalBadge = "THERMAL";

            void recallSnapshot(int sceneIdx) override { lastRecalledScene = sceneIdx; }
            void loadRigFromFile(const juce::File& file, int targetSetlistIndex = -1) override {
                lastLoadedRig = file;
                lastLoadedSetlistIdx = targetSetlistIndex;
            }
            double getCpuUsage() override { return cpuUsage; }
            int getAudioUnderruns() const override { return underruns; }
            bool isThermalWarningActive() const override { return thermalWarn; }
            int getCpuTemperatureCelsius() const override { return cpuTempC; }
            juce::String getThermalBadgeText() const override { return thermalBadge; }
        };

        FanfareEngine eng;
        MockWebHost host;
        OpenRig::WebServer server(eng, &host);

        const auto unknownRes = juce::JSON::parse(
            server.dispatchApiPost("/api/does/not/exist", juce::var()));
        check(unknownRes.isObject() &&
              !(bool)unknownRes.getProperty("ok", true) &&
              unknownRes.getProperty("error", "").toString() == "endpoint not found",
              "Phase3: unknown POST route returns endpoint not found");

        const auto panicRes = juce::JSON::parse(
            server.dispatchApiPost("/api/panic", juce::var()));
        check(panicRes.isObject() && (bool)panicRes.getProperty("ok", false),
              "Phase3: POST /api/panic dispatches cleanly via command table");

        juce::DynamicObject::Ptr badScenePayload = new juce::DynamicObject();
        badScenePayload->setProperty("index", 99999);
        const auto badSceneRes = juce::JSON::parse(
            server.dispatchApiPost("/api/scenes/select",
                                   juce::var(badScenePayload.get())));
        check(badSceneRes.isObject() &&
              !(bool)badSceneRes.getProperty("ok", true),
              "Phase3: POST /api/scenes/select validates bounds via command table");

        void* const handleA = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234));
        void* const handleB = reinterpret_cast<void*>(static_cast<uintptr_t>(0x9999));
        server.registerPluginMirrorSource(nullptr, handleA, "TestPlugin");
        check(server.hasPluginMirrorTarget(),
              "Phase3: registerPluginMirrorSource registers active target");

        server.unregisterPluginMirrorSource(nullptr, handleB);
        check(server.hasPluginMirrorTarget(),
              "Phase3: unregisterPluginMirrorSource ignores mismatched window handle");

        server.unregisterPluginMirrorSource(nullptr, handleA);
        check(!server.hasPluginMirrorTarget(),
              "Phase3: unregisterPluginMirrorSource clears matching window handle");
    }

    // Item 2: CpuTopology & GlitchTelemetry extracted from MainComponent
    {
        check(Fanfare::countBitsInMask(0b10110ULL) == 3,
              "Phase3: countBitsInMask counts set bits accurately");
        const auto topo = Fanfare::queryCpuTopology();
#ifdef _WIN32
        check(topo.sysMask != 0 && !topo.physicalCoreMasks.empty(),
              "Phase3: queryCpuTopology detects Windows processor masks");
#else
        (void)topo;
        check(true, "Phase3: queryCpuTopology compiles cleanly off-Windows");
#endif

        Fanfare::GlitchTelemetry telemetry;
        const juce::int64 t0 = juce::Time::getHighResolutionTicks();
        const juce::int64 normalStep =
            juce::Time::secondsToHighResolutionTicks(0.0029);
        const juce::int64 glitchStep =
            juce::Time::secondsToHighResolutionTicks(0.050);
        const juce::int64 sleepStep =
            juce::Time::secondsToHighResolutionTicks(6.0);

        telemetry.recordCallbackArrival(t0, 128, 44100.0, false, 0);
        telemetry.recordCallbackDurationMs(1.1, 2.9);
        telemetry.recordCallbackArrival(t0 + normalStep, 128, 44100.0, false, 0);
        telemetry.recordCallbackDurationMs(1.2, 2.9);
        check(telemetry.getAudioUnderruns() == 0,
              "Phase3: normal callback interval records zero underruns");

        const juce::int64 tGlitch = t0 + normalStep + glitchStep;
        telemetry.recordCallbackArrival(tGlitch, 128, 44100.0, false, 0);
        double actual = 0.0, expected = 0.0, duration = 0.0;
        const bool hadPending = telemetry.consumePendingGlitch(actual, expected, duration);
        check(telemetry.getAudioUnderruns() == 1 && hadPending &&
              actual >= 45.0 && expected > 2.0 && duration > 1.0,
              "Phase3: stalled callback increments underrun count and captures pending glitch");

        int late = 0, heavy = 0;
        double maxInt = 0.0, maxDur = 0.0;
        const bool hadJitter = telemetry.drainJitterStats(late, heavy, maxInt, maxDur);
        check(hadJitter && late == 1 && heavy == 0 && maxInt >= 45.0,
              "Phase3: drainJitterStats returns late callback count and max interval");

        // Multi-second OS sleep/hibernate gap (>5000ms with <500ms callback duration) must not increment underruns
        telemetry.recordCallbackArrival(tGlitch + sleepStep, 128, 44100.0, false, 0);
        check(telemetry.getAudioUnderruns() == 1,
              "Phase3: sleep/suspend timestamp gap suppresses fake underrun");

        const auto tmpLog = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile("fanfare_selftest_glitch_log.txt");
        tmpLog.deleteFile();
        telemetry.logAudioGlitch(50.0, 2.9, 1.2, "CPU: 12%", "RAM: 500MB", "OK");
        telemetry.logAudioNanSpike(1, 42, 0.0f);
        telemetry.flushGlitchLog(tmpLog);
        const juce::String logText = tmpLog.loadFileAsString();
        check(tmpLog.existsAsFile() &&
              logText.contains("[AUDIO UNDERRUN GLITCH]") &&
              logText.contains("[SAMPLE CORRUPTION / NAN SPIKE]"),
              "Phase3: GlitchTelemetry batches and flushes glitch + NaN entries to disk");
        telemetry.flushGlitchLog(tmpLog);
        check(tmpLog.loadFileAsString() == logText,
              "Phase3: second flushGlitchLog is a no-op once buffer is drained");
        tmpLog.deleteFile();
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: selftest <song.json> [--parse-only] | --web <port> | --unit | --offline-render\n");
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
        runDefectRegressionTests(check);
        runOfflineRenderTest(check);
        runPhase2Tests(check);
        runPhase3Tests(check);
        mm->stopDispatchLoop();
        pump.join();
        return failures ? 1 : 0;
    }

    if (juce::String(argv[1]) == "--offline-render") {
        runOfflineRenderTest(check);
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
