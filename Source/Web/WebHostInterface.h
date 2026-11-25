#pragma once

#include <JuceHeader.h>

namespace OpenRig {

/**
 * Narrow host interface consumed by WebServer so the HTTP/WebSocket layer does
 * not depend directly on MainComponent (and can be unit-tested with a mock).
 */
class IWebHost {
public:
    virtual ~IWebHost() = default;

    virtual void recallSnapshot(int sceneIdx) = 0;
    virtual void loadRigFromFile(const juce::File& file, int targetSetlistIndex = -1) = 0;
    virtual double getCpuUsage() = 0;
    virtual int getAudioUnderruns() const = 0;
    virtual bool isThermalWarningActive() const = 0;
    virtual int getCpuTemperatureCelsius() const = 0;
    virtual juce::String getThermalBadgeText() const = 0;
};

} // namespace OpenRig
