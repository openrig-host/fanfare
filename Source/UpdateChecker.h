#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <thread>
#include "Version.h"

#ifdef _WIN32
#include <windows.h>
#endif

/**
 * Stage-safe one-shot GitHub milestone release checker.
 *
 * Rules for live-rig safety:
 *  - Runs once at startup on a background thread at lowest priority, then exits.
 *  - Short 3-second connection timeout; fails silently when offline or on hotspot Wi-Fi.
 *  - Never shows modal dialogs; invokes callback on the JUCE message thread so UI can
 *    display a subtle header badge.
 *  - Only alerts on major/minor milestone bumps (e.g. 1.4 -> 1.5 or 2.0), ignoring
 *    point/patch updates (e.g. 1.4.1 or 1.41) and pre-releases/drafts.
 */
class UpdateChecker {
public:
  struct ReleaseInfo {
    juce::String tagName;
    juce::String htmlUrl;
    int major = 0;
    int minor = 0;
  };

  UpdateChecker() = default;

  ~UpdateChecker() {
    stop();
  }

  void stop() {
    aborted.store(true, std::memory_order_release);
    if (worker.joinable())
      worker.join();
  }

  static bool parseMilestoneVersion(const juce::String &rawTag,
                                    int localMinor,
                                    int &outMajor,
                                    int &outMinor) {
    juce::String s = rawTag.trim();
    while (s.startsWithChar('v') || s.startsWithChar('V'))
      s = s.substring(1).trim();

    // Strip any -beta / -rc / +build suffix
    int dashIdx = s.indexOfAnyOf("-+ ");
    if (dashIdx >= 0)
      s = s.substring(0, dashIdx);

    juce::StringArray parts;
    parts.addTokens(s, ".", "");
    if (parts.size() < 2)
      return false;

    if (!parts[0].containsOnly("0123456789") || parts[0].isEmpty())
      return false;
    if (!parts[1].containsOnly("0123456789") || parts[1].isEmpty())
      return false;

    outMajor = parts[0].getIntValue();

    if (parts.size() >= 3) {
      // Standard 3-part SemVer: 1.4.1 -> (1, 4), 1.5.0 -> (1, 5), 1.10.0 -> (1, 10)
      outMinor = parts[1].getIntValue();
    } else {
      // 2-part tag: handle both "1.5" -> (1, 5) and decimal patch style "1.41" -> (1, 4)
      if (parts[1].length() >= 2 && localMinor < 10)
        outMinor = parts[1].substring(0, 1).getIntValue();
      else
        outMinor = parts[1].getIntValue();
    }
    return true;
  }

  static bool isNewerMilestone(const juce::String &remoteTag,
                               int localMajor = OPENRIG_VERSION_MAJOR,
                               int localMinor = OPENRIG_VERSION_MINOR,
                               int *outRemoteMajor = nullptr,
                               int *outRemoteMinor = nullptr) {
    int rMaj = 0, rMin = 0;
    if (!parseMilestoneVersion(remoteTag, localMinor, rMaj, rMin))
      return false;
    if (outRemoteMajor) *outRemoteMajor = rMaj;
    if (outRemoteMinor) *outRemoteMinor = rMin;
    return (rMaj > localMajor) || (rMaj == localMajor && rMin > localMinor);
  }

  void checkOnceAsync(std::function<void(const ReleaseInfo &)> onUpdateAvailable) {
    if (hasLaunched.exchange(true))
      return;

    worker = std::thread([this, cb = std::move(onUpdateAvailable)]() {
#ifdef _WIN32
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
      // Brief startup settle so launch I/O and ASIO init have zero competition
      for (int i = 0; i < 20; ++i) {
        if (aborted.load(std::memory_order_acquire))
          return;
        juce::Thread::sleep(100);
      }

      if (aborted.load(std::memory_order_acquire))
        return;

      const juce::URL apiUrl("https://api.github.com/repos/openrig-host/fanfare/releases/latest");
      const juce::String headers =
          "Accept: application/vnd.github+json\r\n"
          "User-Agent: Fanfare-Host/" OPENRIG_VERSION_STRING "\r\n";

      auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                         .withConnectionTimeoutMs(3000)
                         .withExtraHeaders(headers);

      std::unique_ptr<juce::InputStream> stream = apiUrl.createInputStream(options);
      if (stream == nullptr || aborted.load(std::memory_order_acquire))
        return;

      // Cap read size to 64 KB so captive-portal pages cannot stall or bloat memory
      juce::MemoryOutputStream memOut;
      char buf[4096];
      size_t totalRead = 0;
      while (!stream->isExhausted() && totalRead < 65536) {
        if (aborted.load(std::memory_order_acquire))
          return;
        int n = stream->read(buf, (int)sizeof(buf));
        if (n <= 0)
          break;
        memOut.write(buf, (size_t)n);
        totalRead += (size_t)n;
      }

      if (aborted.load(std::memory_order_acquire))
        return;

      juce::var parsed = juce::JSON::parse(memOut.toString());
      auto *obj = parsed.getDynamicObject();
      if (obj == nullptr)
        return;

      if ((bool)obj->getProperty("draft") || (bool)obj->getProperty("prerelease"))
        return;

      const juce::String tag = obj->getProperty("tag_name").toString().trim();
      juce::String htmlUrl = obj->getProperty("html_url").toString().trim();
      if (htmlUrl.isEmpty())
        htmlUrl = "https://github.com/openrig-host/fanfare/releases/latest";

      int rMaj = 0, rMin = 0;
      if (!isNewerMilestone(tag, OPENRIG_VERSION_MAJOR, OPENRIG_VERSION_MINOR, &rMaj, &rMin))
        return;

      ReleaseInfo info;
      info.tagName = tag;
      info.htmlUrl = htmlUrl;
      info.major = rMaj;
      info.minor = rMin;

      if (!aborted.load(std::memory_order_acquire) && cb) {
        juce::MessageManager::callAsync([cb, info]() {
          cb(info);
        });
      }
    });
  }

private:
  std::atomic<bool> hasLaunched{false};
  std::atomic<bool> aborted{false};
  std::thread worker;
};

