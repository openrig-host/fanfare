#pragma once

#include <JuceHeader.h>
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "ControllerProfile.h"
#include "MidiLearnBus.h"
#include "ThemeManager.h"
#include "FanfareConstants.h"

class MidiMonitorComponent : public juce::Component, public juce::Timer {
public:
  struct MidiEvent {
    juce::String text;
    juce::Colour colour;
    juce::int64 timestamp = 0;
    bool isNoteOn = false;
    int noteNumber = -1;
    int velocity = 0;
    int ccNumber = -1;
    int ccValue = 0;
  };

  MidiMonitorComponent() {
    startTimer(30);
    setInterceptsMouseClicks(false, false);
  }

  ~MidiMonitorComponent() override { stopTimer(); }

  void addMidiMessage(const juce::MidiMessage &msg) {
    juce::int64 now = juce::Time::getCurrentTime().toMilliseconds();

    if (msg.isNoteOn()) {
      MidiEvent ev;
      ev.text = "NOTE ON   " +
                juce::MidiMessage::getMidiNoteName(msg.getNoteNumber(), true, true, 4).paddedRight(' ', 4) +
                "  VEL " + juce::String(msg.getVelocity()).paddedLeft('0', 3);
      ev.colour = ThemeManager::get(Theme::Role::midiNote);
      ev.timestamp = now;
      ev.isNoteOn = true;
      ev.noteNumber = msg.getNoteNumber();
      ev.velocity = msg.getVelocity();
      pushEvent(ev);
    } else if (msg.isNoteOff()) {
      MidiEvent ev;
      ev.text = "NOTE OFF  " +
                juce::MidiMessage::getMidiNoteName(msg.getNoteNumber(), true, true, 4);
      ev.colour = ThemeManager::get(Theme::Role::textDim);
      ev.timestamp = now;
      ev.noteNumber = msg.getNoteNumber();
      pushEvent(ev);
    } else if (msg.isController()) {
      MidiEvent ev;
      bool armed = Fanfare::MidiLearnBus::getInstance().isArmed();
      const int ccNum = msg.getControllerNumber();
      const auto desc = Fanfare::ControllerProfile::get().describeCC(ccNum);
      juce::String text = desc.paddedRight(' ', 16) +
                          "  VAL " + juce::String(msg.getControllerValue()).paddedLeft('0', 3);
      if (armed)
        text += "  [CAPTURED]";
      ev.text = text;
      ev.colour = armed ? ThemeManager::get(Theme::Role::foh)
                        : ThemeManager::get(Theme::Role::midiCC);
      ev.timestamp = now;
      ev.ccNumber = msg.getControllerNumber();
      ev.ccValue = msg.getControllerValue();
      pushEvent(ev);
    } else if (msg.isPitchWheel()) {
      MidiEvent ev;
      ev.text = "PITCH     " + juce::String(msg.getPitchWheelValue());
      ev.colour = ThemeManager::get(Theme::Role::midiOther);
      ev.timestamp = now;
      pushEvent(ev);
    } else if (msg.isAftertouch()) {
      MidiEvent ev;
      ev.text = "AFTERTCH  CH " + juce::String(msg.getChannel()) +
                "  VAL " + juce::String(msg.getAfterTouchValue());
      ev.colour = ThemeManager::get(Theme::Role::midiPC);
      ev.timestamp = now;
      pushEvent(ev);
    }
  }

  void paint(juce::Graphics &g) override {
    const auto &t = ThemeManager::getInstance().active();
    Fanfare::ModalFrame::paintRecessedWell(g, getLocalBounds().toFloat().reduced(4.0f, 0.0f), 2.5f);

    int rowH = 18;
    int topOffset = 4;

    // CC-learn banner (zero emoji)
    if (armedLabelNow.isNotEmpty()) {
      g.setColour(t.midiArm);
      g.setFont(BoutiqueLookAndFeel::condensedFont(12.5f, 0.06f));
      g.drawText("LEARNING: " + armedLabelNow.toUpperCase(), 12, topOffset, getWidth() - 24, rowH,
                 juce::Justification::centredLeft, false);
      topOffset += rowH;
    } else if (lastCap.ccNum >= 0 &&
               juce::Time::getCurrentTime().toMilliseconds() - lastCap.timestamp < 2500) {
      g.setColour(t.foh);
      g.setFont(BoutiqueLookAndFeel::condensedFont(12.5f, 0.06f));
      g.drawText("BOUND: " + lastCap.label + " -> CC" + juce::String(lastCap.ccNum),
                 12, topOffset, getWidth() - 24, rowH, juce::Justification::centredLeft, false);
      topOffset += rowH;
    }

    int visibleRows = (getHeight() - topOffset - 4) / rowH;
    g.setFont(BoutiqueLookAndFeel::monoFont(11.5f, false));

    juce::GenericScopedLock<juce::CriticalSection> sl(lock);
    if (displayEvents.empty() && armedLabelNow.isEmpty()) {
      g.setColour(t.textFaint);
      g.setFont(BoutiqueLookAndFeel::uiFont(12.5f, false));
      g.drawText("Waiting for MIDI input...", getLocalBounds().reduced(12),
                 juce::Justification::centred, false);
      return;
    }
    for (int i = 0; i < visibleRows && i < (int)displayEvents.size(); ++i) {
      auto &ev = displayEvents[i];
      float age = (float)(juce::Time::getCurrentTime().toMilliseconds() - ev.timestamp) / 2200.0f;
      float alpha = juce::jmax(0.32f, 1.0f - age);

      int y = topOffset + i * rowH;
      juce::Rectangle<int> row(12, y, getWidth() - 24, rowH - 1);
      bool isCapture = (lastCap.ccNum >= 0 && ev.ccNumber == lastCap.ccNum);

      if (ev.isNoteOn && ev.noteNumber >= 0) {
        float keyPos = (float)ev.noteNumber / 127.0f * (float)(row.getWidth() - 180);
        g.setColour(ev.colour.withAlpha(alpha * 0.22f));
        g.fillRect(row.getX() + 180 + (int)keyPos, row.getY() + 2, 6, row.getHeight() - 4);
        g.setColour(ev.colour.withAlpha(alpha));
        g.drawText(ev.text, row, juce::Justification::centredLeft, false);
      } else if (ev.ccNumber >= 0) {
        float barWidth = (float)ev.ccValue / 127.0f * (float)(row.getWidth() - 180);
        juce::Colour barCol = isCapture ? t.foh.withAlpha(0.28f)
                                        : ev.colour.withAlpha(alpha * 0.18f);
        g.setColour(barCol);
        g.fillRect(row.getX() + 180, row.getY() + 2, (int)barWidth, row.getHeight() - 4);

        g.setColour(isCapture ? t.foh : ev.colour.withAlpha(alpha));
        g.drawText(ev.text, row, juce::Justification::centredLeft, false);
      } else {
        g.setColour(ev.colour.withAlpha(alpha));
        g.drawText(ev.text, row, juce::Justification::centredLeft, false);
      }
    }
  }

  void timerCallback() override {
    bool changed = false;
    {
      juce::GenericScopedLock<juce::CriticalSection> sl(lock);
      if (!pendingEvents.empty()) {
        displayEvents.insert(displayEvents.begin(), pendingEvents.begin(), pendingEvents.end());
        pendingEvents.clear();

        while (displayEvents.size() > 50)
          displayEvents.pop_back();
        changed = true;
      }

      juce::int64 now = juce::Time::getCurrentTime().toMilliseconds();
      while (!displayEvents.empty() && now - displayEvents.back().timestamp > 3000) {
        displayEvents.pop_back();
        changed = true;
      }
    }

    {
      auto &bus = Fanfare::MidiLearnBus::getInstance();
      juce::String newArmed = bus.armedLabel();
      auto newCap = bus.lastCapture();
      if (newArmed != armedLabelNow ||
          (newCap.ccNum != lastCap.ccNum) ||
          (newCap.ccNum >= 0 &&
           juce::Time::getCurrentTime().toMilliseconds() - newCap.timestamp < 2500)) {
        armedLabelNow = newArmed;
        lastCap = newCap;
        changed = true;
      } else {
        armedLabelNow = newArmed;
        lastCap = newCap;
      }
    }

    if (changed)
      repaint();
  }

  juce::CriticalSection &getLock() { return lock; }

private:
  void pushEvent(const MidiEvent &ev) {
    juce::GenericScopedLock<juce::CriticalSection> sl(lock);
    pendingEvents.push_back(ev);
  }

  juce::CriticalSection lock;
  std::vector<MidiEvent> pendingEvents;
  std::vector<MidiEvent> displayEvents;

  juce::String armedLabelNow;
  Fanfare::MidiLearnBus::Capture lastCap;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiMonitorComponent)
};
