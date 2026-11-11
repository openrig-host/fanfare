#pragma once

#include "Scene.h"
#include <JuceHeader.h>
#include <vector>
#include <set>

namespace Fanfare {

struct PluginState {
    juce::String name;
    juce::String path;
    juce::String stateBase64;
    int uid = 0;

    // Per-instrument stacking properties
    int lowNote = 0;
    int highNote = 127;
    float level = 1.0f;
    bool enabled = true;

    // MIDI OUT target (instead of hosting a plugin). When isMidiOut is true
    // this chain slot forwards its filtered MIDI to the named hardware/software
    // output device instead of instantiating a VST. `path`/`state` stay empty.
    bool isMidiOut = false;
    juce::String midiOutDevice;  // device identifier (MidiOutput::getAvailableDevices)
    int midiOutChannel = 1;      // 1..16

    // Built-in SYN-106 Polysynth instance in this chain slot (0..2).
    bool isSynth = false;
    juce::var synthState;

    // Built-in CoplanDX (FM) instance in this chain slot (0..2).
    bool isFm = false;
    juce::var fmState;
};

struct CCMapping {
    int cc = -1;
    int chainIndex = -1;
    juce::String paramId;
    int parameterIndex = -1;
    float minValue = 0.0f;
    float maxValue = 1.0f;
    bool invert = false;
    bool toggle = false;
};

struct CCPassthrough {
    int incomingCC = -1;
    int outgoingCC = -1;
};

struct ChannelStripSettings {
    bool gateEnabled = false;
    float gateThreshold = -40.0f;
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
    int musetteMode = 0; // 0 = 2-Reed MM, 1 = 3-Reed MMM, 2 = TEMU Musette
    float musetteMix = 0.25f;
    int channelMode = 0; // 0 = Stereo, 1 = Mono (L+R), 2 = Mono (Left), 3 = Mono (Right)
};

struct SamplerSlotSettings {
    juce::String wavPath;
    int rootNote = 60;
    int keyLow = 0;
    int keyHigh = 127;
    float pitchOffsetSemitones = 0.0f;
    float volume = 1.0f;
    float startRatio = 0.0f;
    float endRatio = 1.0f;
    bool isLooping = false;
    int volumeCC = -1;
};

struct SamplerSettings {
    bool enabled = false;
    SamplerSlotSettings slots[8];
};

struct Mp3TrackSettings {
    juce::String path;
    juce::String title;
    double duration = 0.0;
};

struct Mp3PlayerSettings {
    std::vector<Mp3TrackSettings> tracks;
    int loopMode = 0;
    bool shuffle = false;
    float gain = 1.0f;
};

// Native synth (FanfareSynthProcessor). The full patch rides as a single
// JSON var produced/consumed by the processor's getState()/applyState().
struct SynthSettings {
    bool enabled = false;
    juce::var state;
};

// Native FM synth (FmSynthProcessor). Same pattern: one JSON var.
struct FmSettings {
    bool enabled = false;
    juce::var state;
};

struct SongSlot {
    juce::String name;
    int iconIndex = 0;
    juce::Colour channelColor{0xff2a2a2a};
    float level = 0.8f;
    bool fohEnabled = true;
    bool iemEnabled = true;
    bool fadersLinked = true;
    bool bypassed = false;
    int inputChannelIndex = -1;
    float aux1Send = 0.0f;
    float aux2Send = 0.0f;
    float iemOffset = 1.0f;
    int transposeOctaves = 0;
    int transposeSemitones = 0;
    int lowNote = 0;
    int highNote = 127;
    std::set<int> allowedCCs;
    int fohCC = -1;
    int iemCC = -1;
    int muteCC = -1;
    int swapCC = -1; // alternates which of swapA/swapB chain slots is enabled
    int swapA = -1;
    int swapB = -1;
    int midiChannelOverride = -1; // -1 means use global default
    int outputTarget = -1;        // -1 = FOH/IEM buses, 0..N = subgroup, >=100 = direct hardware out

    ChannelStripSettings strip;
    std::vector<CCMapping> ccMappings;
    std::vector<CCPassthrough> ccPassthroughs;

struct ArpParams {
    bool enabled = false;
    float bpm = 127.0f;
    int octavesUp = 2;
    int octavesDown = 0;
    float gate = 0.75f;
    int patternIdx = 3; // 0=Up, 1=Down, 2=UpDown, 3=Random
};

struct HarmParams {
    bool enabled = false;
    int octavesUp = 1;
    int octavesDown = 0;
    int africaMode = 0;
    int harmonyTargetSlot = -1; // -1 = local VST, >=0 = route generated notes to this slot
};

    ArpParams arpeggiator;
    HarmParams harmonizer;
    SamplerSettings sampler;
    Mp3PlayerSettings mp3Player;
    SynthSettings synth;
    FmSettings fm;

    std::vector<PluginState> chain;
};

using SlotState = ::SlotState;
using Scene = ::Scene;

struct NoteTab {
    juce::String title;
    juce::String content;
};

struct Song {
    juce::String name;
    float fohMasterLevel = 1.0f;
    float iemMasterLevel = 1.0f;
    int fohOutputOffset = 0;
    int iemOutputOffset = 2;
    int defaultMidiChannel = 1;
    std::vector<PluginState> fohFx;
    std::vector<PluginState> iemFx;
    std::vector<SongSlot> slots;
    std::vector<Scene> scenes;
    int currentSceneIndex = 0;
    std::vector<NoteTab> notes;
    int activeNoteTabIndex = 0;
    float noteFontSize = 20.0f;
    bool noteIsMonospace = true;
    bool freshBuild = false;
};

struct Set {
    juce::String name;
    std::vector<juce::String> songNames; // Or we can store paths/references
    std::vector<Song> songs;
};

} // namespace Fanfare
