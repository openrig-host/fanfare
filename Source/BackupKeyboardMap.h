#pragma once

#include "FanfareConstants.h"
#include <JuceHeader.h>
#include <atomic>

namespace Fanfare {

/**
 * Global "Backup Keyboard" input remap applied at MIDI ingress before any
 * song, split, or CC learn sees the message. Lets you drop in a rented or
 * backup keyboard without editing any saved .json song files.
 */
class BackupKeyboardMap {
public:
  static BackupKeyboardMap &getInstance() {
    static BackupKeyboardMap inst;
    return inst;
  }

  void setEnabled(bool on) {
    enabled.store(on, std::memory_order_release);
    save();
  }
  bool isEnabled() const noexcept { return enabled.load(std::memory_order_acquire); }

  // 0 = Pass-through, 1..16 = Force all incoming channel messages to this channel
  void setForceChannel(int ch) {
    forceChannel.store(juce::jlimit(0, 16, ch), std::memory_order_relaxed);
    save();
  }
  int getForceChannel() const noexcept { return forceChannel.load(std::memory_order_relaxed); }

  void setTransposeSemis(int semis) {
    transposeSemis.store(juce::jlimit(-24, 24, semis), std::memory_order_relaxed);
    save();
  }
  int getTransposeSemis() const noexcept { return transposeSemis.load(std::memory_order_relaxed); }

  void setInvertSustain(bool inv) {
    invertSustain.store(inv, std::memory_order_relaxed);
    save();
  }
  bool isInvertSustain() const noexcept { return invertSustain.load(std::memory_order_relaxed); }

  // Optional single-CC remap (e.g. CC 4 foot controller -> CC 11 expression); -1 = off
  void setRemapSrcCC(int cc) {
    remapSrcCC.store(juce::jlimit(-1, 127, cc), std::memory_order_relaxed);
    save();
  }
  int getRemapSrcCC() const noexcept { return remapSrcCC.load(std::memory_order_relaxed); }

  void setRemapDstCC(int cc) {
    remapDstCC.store(juce::jlimit(0, 127, cc), std::memory_order_relaxed);
    save();
  }
  int getRemapDstCC() const noexcept { return remapDstCC.load(std::memory_order_relaxed); }

  juce::MidiMessage remapIncoming(const juce::MidiMessage &msg) const noexcept {
    if (!enabled.load(std::memory_order_acquire))
      return msg;

    juce::MidiMessage out = msg;
    const int ch = forceChannel.load(std::memory_order_relaxed);
    if (ch >= 1 && ch <= 16 && out.getChannel() > 0)
      out.setChannel(ch);

    if (out.isNoteOn() || out.isNoteOff()) {
      const int semi = transposeSemis.load(std::memory_order_relaxed);
      if (semi != 0)
        out.setNoteNumber(juce::jlimit(0, 127, out.getNoteNumber() + semi));
    } else if (out.isController()) {
      int ccNum = out.getControllerNumber();
      int ccVal = out.getControllerValue();
      const int srcCC = remapSrcCC.load(std::memory_order_relaxed);
      const int dstCC = remapDstCC.load(std::memory_order_relaxed);
      if (srcCC >= 0 && ccNum == srcCC)
        ccNum = dstCC;
      if (ccNum == 64 && invertSustain.load(std::memory_order_relaxed))
        ccVal = 127 - ccVal;
      out = juce::MidiMessage::controllerEvent(juce::jlimit(1, 16, out.getChannel()), ccNum, ccVal);
    }
    return out;
  }

  void load() {
    auto f = FanfareConstants::getAppDirectory().getChildFile("backup_keyboard.json");
    if (!f.existsAsFile())
      return;
    auto v = juce::JSON::parse(f.loadFileAsString());
    if (!v.isObject())
      return;
    enabled.store((bool)v.getProperty("enabled", false), std::memory_order_release);
    forceChannel.store(juce::jlimit(0, 16, (int)v.getProperty("forceChannel", 0)),
                       std::memory_order_relaxed);
    transposeSemis.store(juce::jlimit(-24, 24, (int)v.getProperty("transposeSemis", 0)),
                         std::memory_order_relaxed);
    invertSustain.store((bool)v.getProperty("invertSustain", false),
                        std::memory_order_relaxed);
    remapSrcCC.store(juce::jlimit(-1, 127, (int)v.getProperty("remapSrcCC", -1)),
                     std::memory_order_relaxed);
    remapDstCC.store(juce::jlimit(0, 127, (int)v.getProperty("remapDstCC", 11)),
                     std::memory_order_relaxed);
  }

  void save() const {
    auto f = FanfareConstants::getAppDirectory().getChildFile("backup_keyboard.json");
    auto *obj = new juce::DynamicObject();
    obj->setProperty("enabled", enabled.load(std::memory_order_acquire));
    obj->setProperty("forceChannel", forceChannel.load(std::memory_order_relaxed));
    obj->setProperty("transposeSemis", transposeSemis.load(std::memory_order_relaxed));
    obj->setProperty("invertSustain", invertSustain.load(std::memory_order_relaxed));
    obj->setProperty("remapSrcCC", remapSrcCC.load(std::memory_order_relaxed));
    obj->setProperty("remapDstCC", remapDstCC.load(std::memory_order_relaxed));
    f.getParentDirectory().createDirectory();
    f.replaceWithText(juce::JSON::toString(juce::var(obj)));
  }

private:
  BackupKeyboardMap() = default;

  std::atomic<bool> enabled{false};
  std::atomic<int> forceChannel{0};
  std::atomic<int> transposeSemis{0};
  std::atomic<bool> invertSustain{false};
  std::atomic<int> remapSrcCC{-1};
  std::atomic<int> remapDstCC{11};
};

} // namespace Fanfare
