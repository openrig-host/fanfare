#pragma once

#include "ChannelStripProcessor.h"
#include "OctaveHarmonizer.h"
#include "SimpleArpeggiator.h"
#include "StripState.h"

namespace Fanfare {

inline ChannelStripSettings captureStrip(const FanfareDSP::ChannelStripProcessor& dsp) {
    ChannelStripSettings s;
    s.gateEnabled = dsp.gateEnabled.load();
    s.gateThreshold = dsp.gateThreshold.load();
    s.eqEnabled = dsp.eqEnabled.load();
    s.eqCurveMode = dsp.eqCurveMode.load();
    s.hpfFreq = dsp.hpfFreq.load();
    s.lowShelfGain = dsp.lowShelfGain.load();
    s.highShelfGain = dsp.highShelfGain.load();
    s.eqVolGain = dsp.eqVolGain.load();
    s.eqMasterGain = dsp.eqMasterGain.load();
    for (int b = 0; b < 10; ++b)
        s.eqBands[b] = dsp.eqBands[b].load();
    s.compEnabled = dsp.compEnabled.load();
    s.compAmount = dsp.compAmount.load();
    s.compMakeupDb = dsp.compMakeupDb.load();
    s.compPreset = dsp.compPreset.load();
    s.chorusEnabled = dsp.chorusEnabled.load();
    s.chorusRate = dsp.chorusRate.load();
    s.chorusMix = dsp.chorusMix.load();
    s.delayEnabled = dsp.delayEnabled.load();
    s.delayMode = dsp.delayMode.load();
    s.delayTimeMs = dsp.delayTimeMs.load();
    s.delayFeedback = dsp.delayFeedback.load();
    s.delayTone = dsp.delayTone.load();
    s.delayMix = dsp.delayMix.load();
    s.reverbEnabled = dsp.reverbEnabled.load();
    s.reverbSize = dsp.reverbSize.load();
    s.reverbMix = dsp.reverbMix.load();
    s.irEnabled = dsp.irEnabled.load();
    s.irMix = dsp.irMix.load();
    s.irPath = dsp.getIRReverb().getLoadedPath();
    s.musetteEnabled = dsp.musetteEnabled.load();
    s.musetteCents = dsp.musetteCents.load();
    s.musette3Reed = dsp.musette3Reed.load();
    s.musetteMode = dsp.musetteMode.load();
    s.musetteMix = dsp.musetteMix.load();
    s.channelMode = dsp.channelMode.load();
    return s;
}

inline void applyStrip(FanfareDSP::ChannelStripProcessor& dsp,
                       const ChannelStripSettings& s,
                       bool applyMusetteAndChannelMode = true,
                       bool loadIr = true,
                       bool clearEmptyIr = false) {
    dsp.gateEnabled.store(s.gateEnabled);
    dsp.gateThreshold.store(s.gateThreshold);
    dsp.eqEnabled.store(s.eqEnabled);
    dsp.eqCurveMode.store(s.eqCurveMode);
    dsp.hpfFreq.store(s.hpfFreq);
    dsp.lowShelfGain.store(s.lowShelfGain);
    dsp.highShelfGain.store(s.highShelfGain);
    dsp.eqVolGain.store(s.eqVolGain);
    dsp.eqMasterGain.store(s.eqMasterGain);
    for (int b = 0; b < 10; ++b)
        dsp.eqBands[b].store(s.eqBands[b]);
    dsp.compEnabled.store(s.compEnabled);
    dsp.compAmount.store(s.compAmount);
    dsp.compMakeupDb.store(s.compMakeupDb);
    dsp.compPreset.store(s.compPreset);
    dsp.chorusEnabled.store(s.chorusEnabled);
    dsp.chorusRate.store(s.chorusRate);
    dsp.chorusMix.store(s.chorusMix);
    dsp.delayEnabled.store(s.delayEnabled);
    dsp.delayMode.store(s.delayMode);
    dsp.delayTimeMs.store(s.delayTimeMs);
    dsp.delayFeedback.store(s.delayFeedback);
    dsp.delayTone.store(s.delayTone);
    dsp.delayMix.store(s.delayMix);
    dsp.reverbEnabled.store(s.reverbEnabled);
    dsp.reverbSize.store(s.reverbSize);
    dsp.reverbMix.store(s.reverbMix);
    dsp.irEnabled.store(s.irEnabled);
    dsp.irMix.store(s.irMix);
    if (loadIr) {
        if (s.irPath.isNotEmpty())
            dsp.getIRReverb().loadPathOrPreset(s.irPath);
        else if (clearEmptyIr)
            dsp.getIRReverb().clear();
    }
    if (applyMusetteAndChannelMode) {
        dsp.musetteEnabled.store(s.musetteEnabled);
        dsp.musetteCents.store(s.musetteCents);
        dsp.musette3Reed.store(s.musette3Reed);
        dsp.musetteMode.store(s.musetteMode);
        dsp.musetteMix.store(s.musetteMix);
        dsp.channelMode.store(s.channelMode);
    }
}

inline ArpParams captureArp(const SimpleArpeggiator& arp) {
    ArpParams a;
    a.enabled = arp.enabled.load();
    a.bpm = arp.bpm.load();
    a.octavesUp = arp.octavesUp.load();
    a.octavesDown = arp.octavesDown.load();
    a.gate = arp.gate.load();
    a.patternIdx = arp.patternIdx.load();
    return a;
}

inline void applyArp(SimpleArpeggiator& arp, const ArpParams& a) {
    arp.enabled.store(a.enabled);
    arp.bpm.store(a.bpm);
    arp.octavesUp.store(a.octavesUp);
    arp.octavesDown.store(a.octavesDown);
    arp.gate.store(a.gate);
    arp.patternIdx.store(a.patternIdx);
}

inline HarmParams captureHarm(const OctaveHarmonizer& harm) {
    HarmParams h;
    h.enabled = harm.enabled.load();
    h.octavesUp = harm.octavesUp.load();
    h.octavesDown = harm.octavesDown.load();
    h.africaMode = harm.africaMode.load();
    h.harmonyTargetSlot = harm.harmonyTargetSlot.load();
    return h;
}

inline void applyHarm(OctaveHarmonizer& harm, const HarmParams& h) {
    harm.enabled.store(h.enabled);
    harm.octavesUp.store(h.octavesUp);
    harm.octavesDown.store(h.octavesDown);
    harm.africaMode.store(h.africaMode);
    harm.harmonyTargetSlot.store(h.harmonyTargetSlot);
}

} // namespace Fanfare
