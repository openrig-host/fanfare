#pragma once

#include <JuceHeader.h>
#include "FanfareConstants.h"
#include "StripState.h"
#include <vector>

/**
 * A Scene represents a snapshot of the entire rack's state.
 */
struct SlotState {
  bool bypassed = false;
  float fohLevel = 0.8f;
  float iemLevel = 0.8f;
  bool fohEnabled = true;
  bool iemEnabled = true;
  bool fadersLinked = true;
  float aux1Send = 0.0f;
  float aux2Send = 0.0f;
  float iemOffset = 1.0f;
  int lowNote = 0;
  int highNote = 127;
  int transposeOctaves = 0;
  int transposeSemitones = 0;

  // DSP, Arpeggiator, Harmonizer (canonical unified structs)
  Fanfare::ChannelStripSettings strip;
  bool hasMusetteAndChannelMode = false;
  Fanfare::ArpParams arp;
  Fanfare::HarmParams harm;

  // Sampler
  bool samplerEnabled = false;

  // CC & MIDI channel overrides (instant snapshot recall, zero VST reload)
  bool hasMidiCcState = false;
  int fohCC = -1;
  int iemCC = -1;
  int muteCC = -1;
  int midiChannelOverride = -1;
};

struct Scene {
  juce::String name;
  std::vector<SlotState> slotStates;

  // Optional MIDI trigger: -1 means "no assignment" (use index-based fallback)
  int midiProgramChange = -1;  // 0-127
  int midiChannel = 0;         // 0 = any channel
  juce::uint32 colorArgb = 0;  // 0 = default accent/neutral

  Scene(const juce::String &n) : name(n) {}
};
