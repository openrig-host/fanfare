#pragma once

#include "FanfareConstants.h"
#include <JuceHeader.h>

namespace Fanfare {

struct ChannelStripSettings {
    bool gateEnabled = false;
    float gateThreshold = -60.0f;
    bool eqEnabled = false;
    bool eqCurveMode = false;
    float hpfFreq = 20.0f;
    float lowShelfGain = 0.0f;
    float highShelfGain = 0.0f;
    float eqVolGain = 0.0f;
    float eqMasterGain = 0.0f;
    float eqBands[10] = {};
    bool compEnabled = false;
    float compAmount = 0.0f;
    float compMakeupDb = 0.0f;
    int compPreset = 0;
    bool chorusEnabled = false;
    float chorusRate = 1.0f;
    float chorusMix = 0.0f;
    bool delayEnabled = false;
    int delayMode = 0; // 0 = Stereo, 1 = Ping-Pong, 2 = Mono, 3 = Tape Warm
    float delayTimeMs = 375.0f;
    float delayFeedback = 0.35f;
    float delayTone = 6500.0f;
    float delayMix = 0.25f;
    bool reverbEnabled = false;
    float reverbSize = 0.5f;
    float reverbMix = 0.0f;
    bool irEnabled = false;
    float irMix = 0.3f;
    juce::String irPath;
    bool musetteEnabled = false;
    float musetteCents = 6.0f;
    bool musette3Reed = false;
    int musetteMode = 0; // 0 = 2-Reed MM, 1 = 3-Reed MMM, 2 = Wide Musette
    float musetteMix = 0.25f;
    int channelMode = 0; // 0 = Stereo, 1 = Mono (L+R), 2 = Mono (Left), 3 = Mono (Right)
};

struct ArpParams {
    bool enabled = false;
    float bpm = FanfareConstants::kDefaultArpBpm;
    int octavesUp = FanfareConstants::kDefaultArpOctavesUp;
    int octavesDown = FanfareConstants::kDefaultArpOctavesDown;
    float gate = FanfareConstants::kDefaultArpGate;
    int patternIdx = FanfareConstants::kDefaultArpPatternIdx; // 0=Up, 1=Down, 2=UpDown, 3=Random
};

struct HarmParams {
    bool enabled = false;
    int octavesUp = 1;
    int octavesDown = 0;
    int africaMode = 0;
    int harmonyTargetSlot = -1; // -1 = local VST, >=0 = route generated notes to this slot
};

inline void writeStripToObject(juce::DynamicObject& obj,
                               const ChannelStripSettings& s,
                               bool includeMusetteAndChannelMode = true) {
    obj.setProperty("gateEnabled", s.gateEnabled);
    obj.setProperty("gateThreshold", (double)s.gateThreshold);
    obj.setProperty("eqEnabled", s.eqEnabled);
    obj.setProperty("eqCurveMode", s.eqCurveMode);
    obj.setProperty("hpfFreq", (double)s.hpfFreq);
    obj.setProperty("eqLow", (double)s.lowShelfGain);
    obj.setProperty("eqHigh", (double)s.highShelfGain);
    obj.setProperty("eqVolGain", (double)s.eqVolGain);
    obj.setProperty("eqMasterGain", (double)s.eqMasterGain);
    {
        juce::Array<juce::var> bandsArr;
        for (int b = 0; b < 10; ++b)
            bandsArr.add((double)s.eqBands[b]);
        obj.setProperty("eqBands", bandsArr);
    }
    obj.setProperty("compEnabled", s.compEnabled);
    obj.setProperty("compAmount", (double)s.compAmount);
    obj.setProperty("compMakeupDb", (double)s.compMakeupDb);
    obj.setProperty("compPreset", s.compPreset);
    obj.setProperty("chorusEnabled", s.chorusEnabled);
    obj.setProperty("chorusRate", (double)s.chorusRate);
    obj.setProperty("chorusMix", (double)s.chorusMix);
    obj.setProperty("delayEnabled", s.delayEnabled);
    obj.setProperty("delayMode", s.delayMode);
    obj.setProperty("delayTimeMs", (double)s.delayTimeMs);
    obj.setProperty("delayFeedback", (double)s.delayFeedback);
    obj.setProperty("delayTone", (double)s.delayTone);
    obj.setProperty("delayMix", (double)s.delayMix);
    obj.setProperty("reverbEnabled", s.reverbEnabled);
    obj.setProperty("reverbSize", (double)s.reverbSize);
    obj.setProperty("reverbMix", (double)s.reverbMix);
    obj.setProperty("irEnabled", s.irEnabled);
    obj.setProperty("irMix", (double)s.irMix);
    obj.setProperty("irPath", s.irPath);

    if (includeMusetteAndChannelMode) {
        obj.setProperty("musetteEnabled", s.musetteEnabled);
        obj.setProperty("musetteCents", (double)s.musetteCents);
        obj.setProperty("musette3Reed", (bool)(s.musetteMode == 1));
        obj.setProperty("musetteMode", s.musetteMode);
        obj.setProperty("musetteMix", (double)s.musetteMix);
        obj.setProperty("channelMode", s.channelMode);
    }
}

inline void readStripFromObject(const juce::var& v,
                                ChannelStripSettings& out,
                                bool readMusetteAndChannelMode = true) {
    if (!v.isObject())
        return;

    out.gateEnabled = v.getProperty("gateEnabled", out.gateEnabled);
    out.gateThreshold = (float)v.getProperty("gateThreshold", (double)out.gateThreshold);
    out.eqEnabled = v.getProperty("eqEnabled", out.eqEnabled);
    out.eqCurveMode = v.getProperty("eqCurveMode", out.eqCurveMode);
    out.hpfFreq = (float)v.getProperty("hpfFreq", (double)out.hpfFreq);
    out.lowShelfGain = (float)v.getProperty("eqLow", (double)out.lowShelfGain);
    out.highShelfGain = (float)v.getProperty("eqHigh", (double)out.highShelfGain);
    out.eqVolGain = (float)v.getProperty("eqVolGain", (double)out.eqVolGain);
    out.eqMasterGain = (float)v.getProperty("eqMasterGain", (double)out.eqMasterGain);
    if (auto* bandsArr = v.getProperty("eqBands", juce::var()).getArray()) {
        for (int b = 0; b < 10 && b < bandsArr->size(); ++b)
            out.eqBands[b] = (float)bandsArr->getReference(b);
    }
    out.compEnabled = v.getProperty("compEnabled", out.compEnabled);
    out.compAmount = (float)v.getProperty("compAmount", (double)out.compAmount);
    out.compMakeupDb = (float)v.getProperty("compMakeupDb", (double)out.compMakeupDb);
    out.compPreset = (int)v.getProperty("compPreset", out.compPreset);
    out.chorusEnabled = v.getProperty("chorusEnabled", out.chorusEnabled);
    out.chorusRate = (float)v.getProperty("chorusRate", (double)out.chorusRate);
    out.chorusMix = (float)v.getProperty("chorusMix", (double)out.chorusMix);
    out.delayEnabled = v.getProperty("delayEnabled", out.delayEnabled);
    out.delayMode = (int)v.getProperty("delayMode", out.delayMode);
    out.delayTimeMs = (float)v.getProperty("delayTimeMs", (double)out.delayTimeMs);
    out.delayFeedback = (float)v.getProperty("delayFeedback", (double)out.delayFeedback);
    out.delayTone = (float)v.getProperty("delayTone", (double)out.delayTone);
    out.delayMix = (float)v.getProperty("delayMix", (double)out.delayMix);
    out.reverbEnabled = v.getProperty("reverbEnabled", out.reverbEnabled);
    out.reverbSize = (float)v.getProperty("reverbSize", (double)out.reverbSize);
    out.reverbMix = (float)v.getProperty("reverbMix", (double)out.reverbMix);
    out.irEnabled = v.getProperty("irEnabled", out.irEnabled);
    out.irMix = (float)v.getProperty("irMix", (double)out.irMix);
    out.irPath = v.getProperty("irPath", out.irPath).toString();

    if (readMusetteAndChannelMode) {
        out.musetteEnabled = v.getProperty("musetteEnabled", out.musetteEnabled);
        out.musetteCents = (float)v.getProperty("musetteCents", (double)out.musetteCents);
        out.musette3Reed = v.getProperty("musette3Reed", out.musette3Reed);
        if (v.hasProperty("musetteMode"))
            out.musetteMode = (int)v.getProperty("musetteMode", out.musetteMode);
        else
            out.musetteMode = out.musette3Reed ? 1 : 0;
        out.musetteMix = (float)v.getProperty("musetteMix", (double)out.musetteMix);
        out.channelMode = (int)v.getProperty("channelMode", out.channelMode);
    }
}

inline void writeArpToObject(juce::DynamicObject& obj,
                             const ArpParams& a,
                             bool scenePrefixed = false) {
    obj.setProperty(scenePrefixed ? "arpEnabled" : "enabled", a.enabled);
    obj.setProperty(scenePrefixed ? "arpBpm" : "bpm", (double)a.bpm);
    obj.setProperty(scenePrefixed ? "arpOctavesUp" : "octavesUp", a.octavesUp);
    obj.setProperty(scenePrefixed ? "arpOctavesDown" : "octavesDown", a.octavesDown);
    obj.setProperty(scenePrefixed ? "arpGate" : "gate", (double)a.gate);
    obj.setProperty(scenePrefixed ? "arpPatternIdx" : "patternIdx", a.patternIdx);
}

inline void readArpFromObject(const juce::var& v,
                            ArpParams& out,
                            bool scenePrefixed = false) {
    if (!v.isObject())
        return;

    out.enabled = v.getProperty(scenePrefixed ? "arpEnabled" : "enabled", false);
    out.bpm = (float)v.getProperty(scenePrefixed ? "arpBpm" : "bpm",
                                   (double)FanfareConstants::kDefaultArpBpm);
    out.octavesUp = (int)v.getProperty(scenePrefixed ? "arpOctavesUp" : "octavesUp",
                                       FanfareConstants::kDefaultArpOctavesUp);
    out.octavesDown = (int)v.getProperty(scenePrefixed ? "arpOctavesDown" : "octavesDown",
                                         FanfareConstants::kDefaultArpOctavesDown);
    out.gate = (float)v.getProperty(scenePrefixed ? "arpGate" : "gate",
                                    (double)FanfareConstants::kDefaultArpGate);
    out.patternIdx = (int)v.getProperty(scenePrefixed ? "arpPatternIdx" : "patternIdx",
                                        FanfareConstants::kDefaultArpPatternIdx);
}

inline void writeHarmToObject(juce::DynamicObject& obj,
                              const HarmParams& h,
                              bool scenePrefixed = false) {
    obj.setProperty(scenePrefixed ? "harmEnabled" : "enabled", h.enabled);
    obj.setProperty(scenePrefixed ? "harmOctavesUp" : "octavesUp", h.octavesUp);
    obj.setProperty(scenePrefixed ? "harmOctavesDown" : "octavesDown", h.octavesDown);
    obj.setProperty(scenePrefixed ? "harmAfricaMode" : "africaMode", h.africaMode);
    obj.setProperty(scenePrefixed ? "harmTargetSlot" : "harmonyTargetSlot", h.harmonyTargetSlot);
}

inline void readHarmFromObject(const juce::var& v,
                             HarmParams& out,
                             bool scenePrefixed = false) {
    if (!v.isObject())
        return;

    out.enabled = v.getProperty(scenePrefixed ? "harmEnabled" : "enabled", false);
    out.octavesUp = (int)v.getProperty(scenePrefixed ? "harmOctavesUp" : "octavesUp", 1);
    out.octavesDown = (int)v.getProperty(scenePrefixed ? "harmOctavesDown" : "octavesDown", 0);
    out.africaMode = (int)v.getProperty(scenePrefixed ? "harmAfricaMode" : "africaMode", 0);
    out.harmonyTargetSlot = (int)v.getProperty(scenePrefixed ? "harmTargetSlot" : "harmonyTargetSlot", -1);
}

} // namespace Fanfare
