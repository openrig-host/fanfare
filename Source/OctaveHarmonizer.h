#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <algorithm>

class OctaveHarmonizer {
public:
    OctaveHarmonizer() {
        std::fill(std::begin(localRefCount), std::end(localRefCount), 0);
        std::fill(std::begin(routedRefCount), std::end(routedRefCount), 0);
        std::fill(std::begin(africa2OffsetPerNote), std::end(africa2OffsetPerNote), (int8_t)0);
        outputScratch.ensureSize(2048);
        targetOutputScratch.ensureSize(2048);
    }

    void prepare(double /*sr*/) {
        outputScratch.ensureSize(2048);
        targetOutputScratch.ensureSize(2048);
    }

    void reset() {
        // Schedules a flush on the next audio callback. Safe to call from any thread.
        needsFlush.store(true);
    }

    void processBlock(juce::MidiBuffer& midi, int /*numSamples*/,
                      juce::MidiBuffer* harmonyOutput = nullptr,
                      int transposeSemis = 0) {
        const bool isEnabled = enabled.load();
        bool hasRouteTarget = (harmonyTargetSlot.load() >= 0 && harmonyOutput != nullptr);
        const bool routeChanged = (hasRouteTarget != prevHadRouteTarget);
        prevHadRouteTarget = hasRouteTarget;
        const bool disabledEdge = (!isEnabled && prevEnabled);
        prevEnabled = isEnabled;

        const bool doFlush = needsFlush.exchange(false) || routeChanged || disabledEdge;

        if (!isEnabled && !doFlush)
            return;

        outputScratch.clear();
        targetOutputScratch.clear();

        // Generated harmony voices go to destinationBuffer. When a route target
        // is active they are diverted to targetOutputScratch (forwarded to another
        // slot); otherwise they stay local in outputScratch.
        juce::MidiBuffer& destinationBuffer = hasRouteTarget ? targetOutputScratch : outputScratch;
        int* harmRef = hasRouteTarget ? routedRefCount : localRefCount;

        // --- Flush any stuck generated notes (mode change, route change, enable toggle, panic) ---
        if (doFlush) {
            for (int note = 0; note < 128; ++note) {
                if (localRefCount[note] > 0) {
                    outputScratch.addEvent(juce::MidiMessage::noteOff(1, note), 0);
                }
                if (routedRefCount[note] > 0) {
                    if (harmonyOutput != nullptr)
                        targetOutputScratch.addEvent(juce::MidiMessage::noteOff(1, note), 0);
                    outputScratch.addEvent(juce::MidiMessage::noteOff(1, note), 0);
                }
            }
            std::fill(std::begin(localRefCount), std::end(localRefCount), 0);
            std::fill(std::begin(routedRefCount), std::end(routedRefCount), 0);
            std::fill(std::begin(africa2OffsetPerNote), std::end(africa2OffsetPerNote), (int8_t)0);
        }

        if (!isEnabled) {
            // Pass all incoming MIDI through alongside the flush note-offs
            for (const auto meta : midi)
                outputScratch.addEvent(meta.getMessage(), meta.samplePosition);
            if (harmonyOutput != nullptr)
                harmonyOutput->addEvents(targetOutputScratch, 0, -1, 0);
            midi.swapWith(outputScratch);
            return;
        }

        int up = octavesUp.load();
        int down = octavesDown.load();
        int africa = africaMode.load();

        auto addLocalNote = [&](int genNote, int vel, int pos) {
            if (genNote >= 0 && genNote < 128) {
                if (localRefCount[genNote] == 0) {
                    outputScratch.addEvent(juce::MidiMessage::noteOn(1, genNote, (juce::uint8)vel), pos);
                }
                localRefCount[genNote]++;
            }
        };

        auto addHarmonyNote = [&](int genNote, int vel, int pos) {
            if (genNote >= 0 && genNote < 128) {
                if (harmRef[genNote] == 0) {
                    destinationBuffer.addEvent(juce::MidiMessage::noteOn(1, genNote, (juce::uint8)vel), pos);
                }
                harmRef[genNote]++;
            }
        };

        auto releaseLocalNote = [&](int genNote, int pos) {
            if (genNote >= 0 && genNote < 128) {
                if (localRefCount[genNote] > 0) {
                    localRefCount[genNote]--;
                    if (localRefCount[genNote] == 0) {
                        outputScratch.addEvent(juce::MidiMessage::noteOff(1, genNote), pos);
                    }
                }
            }
        };

        auto releaseHarmonyNote = [&](int genNote, int pos) {
            if (genNote >= 0 && genNote < 128) {
                if (harmRef[genNote] > 0) {
                    harmRef[genNote]--;
                    if (harmRef[genNote] == 0) {
                        destinationBuffer.addEvent(juce::MidiMessage::noteOff(1, genNote), pos);
                    }
                }
            }
        };

        for (const auto meta : midi) {
            auto msg = meta.getMessage();
            int pos = meta.samplePosition;

            if (msg.isNoteOn() && msg.getVelocity() > 0) {
                int note = msg.getNoteNumber();
                int vel = msg.getVelocity();

                if (note < 0 || note >= 128) {
                    outputScratch.addEvent(msg, pos);
                    continue;
                }

                if (africa != 3) {
                    // Pass original note locally
                    addLocalNote(note, vel, pos);
                }

                if (africa == 1) {
                    addHarmonyNote(note - 5, vel, pos);
                } else if (africa == 2) {
                    static constexpr int8_t offsets[12] = { -3, -3, -4, -4, -4, -3, -3, -3, -4, -4, -3, -3 };
                    int pc = (((note - transposeSemis) % 12) + 12) % 12;
                    int8_t off = offsets[pc];
                    africa2OffsetPerNote[note] = off;
                    addHarmonyNote(note + off, vel, pos);
                } else if (africa == 3) {
                    int transposed = note - 1;
                    bool isPart1 = (note >= 48);

                    if (isPart1) {
                        addLocalNote(transposed, vel, pos);
                        addHarmonyNote(transposed - 5, vel, pos);
                    } else {
                        addLocalNote(transposed + 36, vel, pos);
                        addHarmonyNote(transposed - 5 + 36, vel, pos);
                    }
                } else {
                    for (int oct = 1; oct <= up; ++oct) {
                        addHarmonyNote(note + oct * 12, vel, pos);
                    }
                    for (int oct = 1; oct <= down; ++oct) {
                        addHarmonyNote(note - oct * 12, vel, pos);
                    }
                }
            } else if (msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0)) {
                int note = msg.getNoteNumber();

                if (note < 0 || note >= 128) {
                    outputScratch.addEvent(msg, pos);
                    continue;
                }

                if (africa != 3) {
                    releaseLocalNote(note, pos);
                }

                if (africa == 1) {
                    releaseHarmonyNote(note - 5, pos);
                } else if (africa == 2) {
                    int off = (int)africa2OffsetPerNote[note];
                    if (off == 0) {
                        static constexpr int8_t offsets[12] = { -3, -3, -4, -4, -4, -3, -3, -3, -4, -4, -3, -3 };
                        int pc = (((note - transposeSemis) % 12) + 12) % 12;
                        off = offsets[pc];
                    }
                    africa2OffsetPerNote[note] = 0;
                    releaseHarmonyNote(note + off, pos);
                } else if (africa == 3) {
                    int transposed = note - 1;
                    bool isPart1 = (note >= 48);

                    if (isPart1) {
                        releaseLocalNote(transposed, pos);
                        releaseHarmonyNote(transposed - 5, pos);
                    } else {
                        releaseLocalNote(transposed + 36, pos);
                        releaseHarmonyNote(transposed - 5 + 36, pos);
                    }
                } else {
                    for (int oct = 1; oct <= up; ++oct) {
                        releaseHarmonyNote(note + oct * 12, pos);
                    }
                    for (int oct = 1; oct <= down; ++oct) {
                        releaseHarmonyNote(note - oct * 12, pos);
                    }
                }
            } else {
                // Pass through all other MIDI (CC, pitch bend, etc.) — always local
                outputScratch.addEvent(msg, pos);
            }
        }

        if (harmonyOutput != nullptr)
            harmonyOutput->addEvents(targetOutputScratch, 0, -1, 0);
        midi.swapWith(outputScratch);
    }

    // Parameters — atomic, safe to write from message thread
    std::atomic<bool> enabled{false};
    std::atomic<int>  octavesUp{1};
    std::atomic<int>  octavesDown{0};
    std::atomic<int>  africaMode{0};
    std::atomic<int>  harmonyTargetSlot{-1}; // -1 = local VST, >=0 = route voices to slot

    // Set this from the message thread whenever you change mode, enable, or want a panic.
    // The next processBlock() call will flush all generated notes cleanly.
    std::atomic<bool> needsFlush{false};

private:
    int localRefCount[128];          // note -> active reference count on local strip
    int routedRefCount[128];         // note -> active reference count on routed target strip
    int8_t africa2OffsetPerNote[128];
    bool prevHadRouteTarget = false;
    bool prevEnabled = false;
    juce::MidiBuffer outputScratch;
    juce::MidiBuffer targetOutputScratch;
};
