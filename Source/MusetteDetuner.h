#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cmath>

namespace FanfareDSP {

/**
 * MusetteDetunerChannel
 *
 * Implements high-precision, low-latency micro-pitch detuning with integrated
 * pre-filtering specifically engineered for accordion reed physics.
 *
 * Architecture:
 * - Dual-Path Detune: Split incoming signal into Dry and Wet paths.
 * - Bit-accurate Dry Path: completely unmodulated to preserve transient snap of
 *   pallets hitting the reed block.
 * - Wet Path Pre-Filtering:
 *     1. 2nd-order Butterworth High-Pass Filter at 85 Hz (suppresses bellows rumble,
 *        handling noise, and key-bed thumps).
 *     2. Peaking Dip at 3.5 kHz (Q = 2.5, -3.0 dB) on wet line only (eliminates
 *        metallic reed rasp doubling and harsh phase-smear sizzle).
 * - Detune Engine:
 *     - No sinusoidal LFO modulation (avoiding unnatural chorus warble).
 *     - Dual-tap rotating circular buffer with raised-cosine crossfade.
 *     - Sharp tap: +6 cents default (+3 to +12 cents range).
 *     - Optional flat tap: -4 cents for 3-reed (MMM) musette / terzetti emulation.
 *     - 4-point Hermite cubic fractional interpolation.
 * - Mix Control:
 *     - Baseline default: 75% Dry / 25% Detuned Wet.
 */
class MusetteDetunerChannel {
public:
    MusetteDetunerChannel() { initHannLut(); }

    void prepare(double sampleRate) {
        if (sampleRate <= 0.0)
            sampleRate = 44100.0;
        sr = sampleRate;
        mixSmoothCoeff = std::exp(-1.0f / juce::jmax(1.0f, 0.010f * (float)sr));

        updateCoefficients();
        cachedSr = -1.0; // Force detune step recomputation on next sample
        reset();
    }

    void reset() {
        std::fill(std::begin(delayBuffer), std::end(delayBuffer), 0.0f);
        writeIndex = 0;
        phaseSharp = 0.0f;
        phaseFlat = 0.25f; // Initial phase offset between voices
        smoothedMix = 0.0f;

        hpfS1 = hpfS2 = 0.0f;
        dipS1 = dipS2 = 0.0f;
    }

    float process(float input, float cents, int mode, float mix) {
        const float targetMix = juce::jlimit(0.0f, 1.0f, mix);
        smoothedMix = targetMix + mixSmoothCoeff * (smoothedMix - targetMix);
        if (targetMix <= 0.0001f && smoothedMix <= 0.0001f) {
            smoothedMix = 0.0f;
            return input;
        }

        // 1. Wet Path Pre-Filtering
        // Step A: 85 Hz 2nd-order Butterworth HPF
        float filtered = processBiquad(input, hpfB0, hpfB1, hpfB2, hpfA1, hpfA2, hpfS1, hpfS2);
        // Step B: 3.5 kHz Peaking Dip (-3 dB, Q = 2.5)
        filtered = processBiquad(filtered, dipB0, dipB1, dipB2, dipA1, dipA2, dipS1, dipS2);

        // 2. Write filtered sample into circular delay buffer
        delayBuffer[writeIndex] = filtered;

        updateDetuneRatesIfNeeded(cents, mode);

        // --- Voice 1: Sharp Reed (+Cents) ---
        phaseSharp += cachedPhaseStepSharp;
        if (phaseSharp < 0.0f)
            phaseSharp += 1.0f;
        else if (phaseSharp >= 1.0f)
            phaseSharp -= 1.0f;

        float wetSharp = readDualTap(phaseSharp, cachedMinDelay, cachedWindowSamples);

        // --- Voice 2: Flat Reed (active in 3-reed MMM and Wide Vibrato modes) ---
        float wetTotal = wetSharp;
        if (cachedHasFlat) {
            phaseFlat += cachedPhaseStepFlat;
            if (phaseFlat < 0.0f)
                phaseFlat += 1.0f;
            else if (phaseFlat >= 1.0f)
                phaseFlat -= 1.0f;

            float wetFlat = readDualTap(phaseFlat, cachedMinDelay, cachedWindowSamples);
            wetTotal = 0.5f * (wetSharp + wetFlat);
        }

        // Advance write pointer
        writeIndex = (writeIndex + 1) & kBufferMask;

        // 3. Dry/Wet Blend: 100% bit-accurate dry path + detuned wet path
        return input * (1.0f - smoothedMix) + wetTotal * smoothedMix;
    }

private:
    static constexpr int kBufferSize = 8192;
    static constexpr int kBufferMask = kBufferSize - 1;
    static constexpr float kTwoPi = 6.28318530717958647692f;
    static constexpr int kHannLutSize = 512;

    void initHannLut() {
        for (int i = 0; i <= kHannLutSize; ++i)
            hannLut[i] = 0.5f * (1.0f - std::cos(kTwoPi * (float)i / (float)kHannLutSize));
    }

    float lookupHann(float phase) const noexcept {
        const float pos = juce::jlimit(0.0f, (float)kHannLutSize, phase * (float)kHannLutSize);
        const int idx = juce::jmin(kHannLutSize - 1, (int)pos);
        const float frac = pos - (float)idx;
        return hannLut[idx] + frac * (hannLut[idx + 1] - hannLut[idx]);
    }

    void updateDetuneRatesIfNeeded(float cents, int mode) {
        if (cents == cachedCents && mode == cachedMode && sr == cachedSr)
            return;

        cachedCents = cents;
        cachedMode = mode;
        cachedSr = sr;
        cachedWindowSamples = juce::jlimit(256.0f, 4096.0f, 0.032f * (float)sr);
        cachedMinDelay = 0.002f * (float)sr;
        cachedHasFlat = (mode == 1 || mode == 2);

        float sharpCents = cents;
        float flatCents = -4.0f;
        if (mode == 2) { // Wide Musette: exaggerated dual-reed vibrato detuning
            sharpCents = juce::jlimit(12.0f, 32.0f, cents * 2.5f);
            flatCents = -juce::jlimit(10.0f, 26.0f, cents * 2.0f);
        }

        const float rSharp = std::pow(2.0f, sharpCents / 1200.0f);
        cachedPhaseStepSharp = (1.0f - rSharp) / cachedWindowSamples;

        if (cachedHasFlat) {
            const float rFlat = std::pow(2.0f, flatCents / 1200.0f);
            cachedPhaseStepFlat = (1.0f - rFlat) / cachedWindowSamples;
        } else {
            cachedPhaseStepFlat = 0.0f;
        }
    }

    float readDualTap(float phase, float minDelay, float windowSamples) const {
        // Tap 1
        float p1 = phase;
        float d1 = minDelay + p1 * windowSamples;
        float w1 = lookupHann(p1);
        float s1 = readHermite(d1);

        // Tap 2 (180 deg / 0.5 out of phase)
        float p2 = phase + 0.5f;
        if (p2 >= 1.0f)
            p2 -= 1.0f;
        float d2 = minDelay + p2 * windowSamples;
        float w2 = 1.0f - w1;
        float s2 = readHermite(d2);

        return w1 * s1 + w2 * s2;
    }

    float readHermite(float delaySamples) const {
        float readPos = (float)writeIndex - delaySamples;
        int i0 = (int)std::floor(readPos);
        float frac = readPos - (float)i0;

        float ym1 = delayBuffer[(i0 - 1) & kBufferMask];
        float y0  = delayBuffer[(i0)     & kBufferMask];
        float y1  = delayBuffer[(i0 + 1) & kBufferMask];
        float y2  = delayBuffer[(i0 + 2) & kBufferMask];

        // 4-point Hermite cubic interpolation
        float c0 = y0;
        float c1 = 0.5f * (y1 - ym1);
        float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);

        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    static float processBiquad(float in, float b0, float b1, float b2,
                               float a1, float a2, float &s1, float &s2) {
        float out = b0 * in + s1;
        s1 = b1 * in - a1 * out + s2;
        s2 = b2 * in - a2 * out;
        if (std::abs(s1) < 1.0e-15f) s1 = 0.0f;
        if (std::abs(s2) < 1.0e-15f) s2 = 0.0f;
        if (std::abs(out) < 1.0e-15f)
            out = 0.0f; // anti-denormal flush
        return out;
    }

    void updateCoefficients() {
        // 1. 2nd-order Butterworth HPF at 85 Hz (Q = 1/sqrt(2) ~= 0.70710678)
        {
            const float f0 = 85.0f;
            const float w0 = kTwoPi * (f0 / (float)sr);
            const float cosW = std::cos(w0);
            const float sinW = std::sin(w0);
            const float alpha = sinW * 0.70710678f;

            const float b0 = (1.0f + cosW) * 0.5f;
            const float b1 = -(1.0f + cosW);
            const float b2 = (1.0f + cosW) * 0.5f;
            const float a0 = 1.0f + alpha;
            const float a1 = -2.0f * cosW;
            const float a2 = 1.0f - alpha;

            hpfB0 = b0 / a0;
            hpfB1 = b1 / a0;
            hpfB2 = b2 / a0;
            hpfA1 = a1 / a0;
            hpfA2 = a2 / a0;
        }

        // 2. Peaking Dip at 3.5 kHz (Q = 2.5, -3.0 dB gain)
        {
            const float f0 = 3500.0f;
            const float gainDb = -3.0f;
            const float A = std::pow(10.0f, gainDb / 40.0f);
            const float w0 = kTwoPi * (f0 / (float)sr);
            const float cosW = std::cos(w0);
            const float sinW = std::sin(w0);
            const float alpha = sinW / (2.0f * 2.5f);

            const float b0 = 1.0f + alpha * A;
            const float b1 = -2.0f * cosW;
            const float b2 = 1.0f - alpha * A;
            const float a0 = 1.0f + alpha / A;
            const float a1 = -2.0f * cosW;
            const float a2 = 1.0f - alpha / A;

            dipB0 = b0 / a0;
            dipB1 = b1 / a0;
            dipB2 = b2 / a0;
            dipA1 = a1 / a0;
            dipA2 = a2 / a0;
        }
    }

    double sr = 44100.0;
    float delayBuffer[kBufferSize] = {};
    int writeIndex = 0;
    float phaseSharp = 0.0f;
    float phaseFlat = 0.25f;

    float mixSmoothCoeff = 0.99f;
    float smoothedMix = 0.0f;

    double cachedSr = -1.0;
    float cachedCents = -1.0f;
    int cachedMode = -1;
    float cachedWindowSamples = 1411.2f;
    float cachedMinDelay = 88.2f;
    float cachedPhaseStepSharp = 0.0f;
    float cachedPhaseStepFlat = 0.0f;
    bool cachedHasFlat = false;
    float hannLut[kHannLutSize + 1] = {};

    // Filter states & coefficients
    float hpfB0 = 1.0f, hpfB1 = 0.0f, hpfB2 = 0.0f, hpfA1 = 0.0f, hpfA2 = 0.0f;
    float hpfS1 = 0.0f, hpfS2 = 0.0f;

    float dipB0 = 1.0f, dipB1 = 0.0f, dipB2 = 0.0f, dipA1 = 0.0f, dipA2 = 0.0f;
    float dipS1 = 0.0f, dipS2 = 0.0f;
};

/**
 * Stereo wrapper for MusetteDetuner
 */
class MusetteDetuner {
public:
    void prepare(double sampleRate) {
        channelL.prepare(sampleRate);
        channelR.prepare(sampleRate);
    }

    void reset() {
        channelL.reset();
        channelR.reset();
    }

    void process(float &left, float &right, bool isStereo, float cents, int mode, float mix) {
        left = channelL.process(left, cents, mode, mix);
        if (isStereo)
            right = channelR.process(right, cents, mode, mix);
    }

    void processMono(float &sample, float cents, int mode, float mix) {
        sample = channelL.process(sample, cents, mode, mix);
    }

private:
    MusetteDetunerChannel channelL;
    MusetteDetunerChannel channelR;
};

} // namespace FanfareDSP
