#pragma once

#include "Logger.h"
#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <map>
#include <queue>

class YoutubeDownloadManager : public juce::Thread {
public:
  struct VideoInfo {
    bool success = false;
    juce::String title;
    juce::String uploader;
    juce::String duration;
    juce::String url;
    juce::String errorMessage;
  };

  struct DownloadJob {
    juce::String url;
    bool playImmediately = false;
    juce::File destinationDir;
    std::function<void(bool success, const juce::File &file,
                       const juce::String &title, const juce::String &error)>
        onComplete;
    std::function<void(const juce::String &status, float progress)> onProgress;
  };

  static YoutubeDownloadManager &getInstance() {
    static YoutubeDownloadManager instance;
    return instance;
  }

  static bool isValidDownloadUrl(const juce::String &rawUrl) {
    auto u = rawUrl.trim();
    if (u.isEmpty() || u.startsWithChar('-') || u.length() > 2048)
      return false;
    if (!(u.startsWithIgnoreCase("https://") ||
          u.startsWithIgnoreCase("http://")))
      return false;
    for (int i = 0; i < u.length(); ++i) {
      auto ch = u[i];
      if (ch <= 0x20 || ch == 0x7f)
        return false;
    }

    // SSRF protection: reject localhost, loopback, link-local, private RFC1918, and cloud metadata addresses
    juce::URL parsed(u);
    juce::String host = parsed.getDomain().toLowerCase();
    if (host.isEmpty())
      return false;
    if (host == "localhost" || host.endsWith(".localhost") || host == "127.0.0.1" ||
        host.startsWith("127.") || host == "::1" || host == "[::1]" || host.contains("::"))
      return false;
    if (host.startsWith("10.") || host.startsWith("192.168.") ||
        host.startsWith("169.254.") || host.startsWith("0."))
      return false;
    if (host.startsWith("172.")) {
      int secondDot = host.indexOfChar(4, '.');
      if (secondDot > 4) {
        int octet2 = host.substring(4, secondDot).getIntValue();
        if (octet2 >= 16 && octet2 <= 31)
          return false;
      }
    }
    juce::IPAddress ip(host);
    if (!ip.isNull()) {
      const uint8_t a = ip.address[0];
      const uint8_t b = ip.address[1];
      if (a == 0 || a == 127 || a == 10 || (a == 169 && b == 254) || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31))
        return false;
    }
    return true;
  }

  static void
  fetchVideoInfoAsync(const juce::String &inputUrl,
                      std::function<void(const VideoInfo &)> callback) {
    juce::Thread::launch([url = inputUrl.trim(), callback]() {
      VideoInfo info;
      info.url = url;

      if (!isValidDownloadUrl(url)) {
        info.errorMessage =
            "Invalid URL: only http:// and https:// links are allowed.";
        juce::MessageManager::callAsync([callback, info]() {
          if (callback)
            callback(info);
        });
        return;
      }

      // 1. Try YouTube oEmbed API first (<200ms quick response)
      if (url.contains("youtube.com") || url.contains("youtu.be")) {
        juce::URL oembedUrl("https://www.youtube.com/oembed?url=" +
                            juce::URL::addEscapeChars(url, true) +
                            "&format=json");
        std::unique_ptr<juce::InputStream> stream(oembedUrl.createInputStream(
            juce::URL::InputStreamOptions(
                juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs(3000)));
        if (stream != nullptr) {
          auto jsonStr = stream->readEntireStreamAsString();
          auto parsed = juce::JSON::parse(jsonStr);
          if (parsed.isObject()) {
            info.title = parsed.getProperty("title", "").toString();
            info.uploader = parsed.getProperty("author_name", "").toString();
            if (info.title.isNotEmpty()) {
              info.success = true;
            }
          }
        }
      }

      // 2. If oEmbed didn't work, use yt-dlp --print
      if (!info.success) {
        auto ytDlp = findYtDlpExe();
        if (ytDlp.existsAsFile()) {
          juce::StringArray args;
          args.add(ytDlp.getFullPathName());
          args.add("--print");
          args.add("%(title)s|||%(duration_string)s|||%(uploader)s");
          args.add("--no-playlist");
          args.add("--no-warnings");
          args.add("--"); // end of options: URL is a positional only
          args.add(url);

          juce::ChildProcess p;
          if (p.start(args, juce::ChildProcess::wantStdOut)) {
            auto out = p.readAllProcessOutput().trim();
            p.waitForProcessToFinish(6000);
            if (out.isNotEmpty()) {
              auto firstLine =
                  out.upToFirstOccurrenceOf("\n", false, false).trim();
              auto titlePart =
                  firstLine.upToFirstOccurrenceOf("|||", false, false).trim();
              if (titlePart.isNotEmpty()) {
                info.title = titlePart;
                if (firstLine.contains("|||")) {
                  auto rest =
                      firstLine.fromFirstOccurrenceOf("|||", false, false);
                  info.duration =
                      rest.upToFirstOccurrenceOf("|||", false, false).trim();
                  if (rest.contains("|||"))
                    info.uploader =
                        rest.fromFirstOccurrenceOf("|||", false, false).trim();
                }
                info.success = true;
              }
            }
          }
        }
      }

      if (!info.success && info.errorMessage.isEmpty()) {
        info.errorMessage = "Could not identify song title for this link.";
      }

      juce::MessageManager::callAsync([callback, info]() {
        if (callback)
          callback(info);
      });
    });
  }

  static juce::File getToolsDir() {
    auto dir =
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Fanfare")
            .getChildFile("Tools");
    if (!dir.exists())
      dir.createDirectory();
    return dir;
  }

  static juce::File getDefaultDownloadsDir() {
    auto dir =
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Fanfare")
            .getChildFile("Downloads");
    if (!dir.exists())
      dir.createDirectory();
    return dir;
  }

  juce::File getDownloadsDir() const {
    juce::ScopedLock sl(configLock);
    if (customDownloadsDir.isDirectory())
      return customDownloadsDir;
    return getDefaultDownloadsDir();
  }

  void setDownloadsDir(const juce::File &dir) {
    if (dir.isDirectory()) {
      juce::ScopedLock sl(configLock);
      customDownloadsDir = dir;
      saveConfig();
    }
  }

  static void openFolderInExplorer(const juce::File &folder) {
    auto target = folder.isDirectory() ? folder : getDefaultDownloadsDir();
    if (target.exists()) {
      target.startAsProcess();
    }
  }

  static juce::File findYtDlpExe() {
    // 1. AppData Tools
    auto appDataExe = getToolsDir().getChildFile("yt-dlp.exe");
    if (appDataExe.existsAsFile())
      return appDataExe;

    // 2. Next to executable
    auto exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getParentDirectory();
    auto localExe = exeDir.getChildFile("yt-dlp.exe");
    if (localExe.existsAsFile())
      return localExe;

    return appDataExe;
  }

  static juce::File findFfmpegExe() {
    auto appDataExe = getToolsDir().getChildFile("ffmpeg.exe");
    if (appDataExe.existsAsFile())
      return appDataExe;

    auto exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getParentDirectory();
    auto localExe = exeDir.getChildFile("ffmpeg.exe");
    if (localExe.existsAsFile())
      return localExe;

    return appDataExe;
  }

  void queueDownload(
      const juce::String &url, bool playImmediately,
      std::function<void(bool, const juce::File &, const juce::String &,
                         const juce::String &)>
          onComplete,
      std::function<void(const juce::String &, float)> onProgress = nullptr) {
    queueDownload(url, playImmediately, juce::File(), onComplete, onProgress);
  }

  void queueDownload(
      const juce::String &url, bool playImmediately,
      const juce::File &customTargetDir = {},
      std::function<void(bool, const juce::File &, const juce::String &,
                         const juce::String &)>
          onComplete = nullptr,
      std::function<void(const juce::String &, float)> onProgress = nullptr) {
    if (!isValidDownloadUrl(url)) {
      if (onComplete) {
        juce::MessageManager::callAsync([onComplete]() {
          onComplete(
              false, juce::File(), "",
              "Invalid URL: only http:// and https:// links are allowed.");
        });
      }
      return;
    }

    DownloadJob job;
    job.url = url.trim();
    job.playImmediately = playImmediately;
    job.destinationDir =
        customTargetDir.isDirectory() ? customTargetDir : getDownloadsDir();
    job.onComplete = onComplete;
    job.onProgress = onProgress;

    {
      juce::ScopedLock sl(queueLock);
      jobQueue.push(job);
    }

    if (!isThreadRunning()) {
      startThread(juce::Thread::Priority::normal);
    } else {
      notify();
    }
  }

  bool isDownloading() const { return activeDownload.load(); }

  juce::String getCurrentStatus() const {
    juce::ScopedLock sl(statusLock);
    return currentStatusText;
  }

private:
  YoutubeDownloadManager() : juce::Thread("YoutubeDownloadWorker") {
    loadConfig();
  }

  ~YoutubeDownloadManager() override {
    signalThreadShouldExit();
    notify();
    stopThread(3000);
  }

  void loadConfig() {
    auto file =
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Fanfare")
            .getChildFile("settings")
            .getChildFile("youtube_settings.json");
    if (file.existsAsFile()) {
      auto parsed = juce::JSON::parse(file);
      if (parsed.isObject()) {
        juce::String path =
            parsed.getProperty("downloadsFolder", "").toString();
        if (path.isNotEmpty()) {
          juce::File dir(path);
          if (dir.isDirectory()) {
            customDownloadsDir = dir;
          }
        }
      }
    }
  }

  void saveConfig() {
    auto dir =
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Fanfare")
            .getChildFile("settings");
    if (!dir.exists())
      dir.createDirectory();
    auto file = dir.getChildFile("youtube_settings.json");

    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("downloadsFolder", customDownloadsDir.getFullPathName());
    file.replaceWithText(juce::JSON::toString(juce::var(obj.get())));
  }

  void run() override {
    while (!threadShouldExit()) {
      DownloadJob job;
      bool hasJob = false;

      {
        juce::ScopedLock sl(queueLock);
        if (!jobQueue.empty()) {
          job = jobQueue.front();
          jobQueue.pop();
          hasJob = true;
        }
      }

      if (hasJob) {
        activeDownload.store(true);
        processJob(job);
        activeDownload.store(false);
      } else {
        wait(500);
      }
    }
  }

  void setStatus(const juce::String &text, float progress,
                 const DownloadJob &job) {
    {
      juce::ScopedLock sl(statusLock);
      currentStatusText = text;
    }
    if (job.onProgress) {
      juce::MessageManager::callAsync(
          [cb = job.onProgress, text, progress]() { cb(text, progress); });
    }
  }

  void processJob(const DownloadJob &job) {
    setStatus("Initializing YouTube download...", 0.05f, job);

    auto ytDlp = findYtDlpExe();
    auto ffmpeg = findFfmpegExe();
    auto dlDir = job.destinationDir.isDirectory() ? job.destinationDir
                                                  : getDownloadsDir();
    if (!dlDir.exists())
      dlDir.createDirectory();

    // If yt-dlp.exe is missing, download it on the fly via verified temp file
    if (!ytDlp.existsAsFile()) {
      setStatus("Downloading yt-dlp tool...", 0.10f, job);
      auto tmpFile = ytDlp.getSiblingFile("yt-dlp.exe.tmp");
      tmpFile.deleteFile();
      auto url = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/"
                 "yt-dlp.exe";
      juce::URL downloadUrl(url);
      std::unique_ptr<juce::InputStream> inStream(downloadUrl.createInputStream(
          juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
              .withConnectionTimeoutMs(15000)));
      if (inStream != nullptr) {
        bool writeOk = false;
        {
          juce::FileOutputStream outStream(tmpFile);
          if (outStream.openedOk()) {
            outStream.writeFromInputStream(*inStream, -1);
            outStream.flush();
            writeOk = true;
          }
        }
        bool validPe = false;
        if (writeOk && tmpFile.getSize() >= 4 * 1024 * 1024) {
          juce::FileInputStream checkStream(tmpFile);
          char mz[2] = {0, 0};
          if (checkStream.openedOk() && checkStream.read(mz, 2) == 2 &&
              mz[0] == 'M' && mz[1] == 'Z') {
            validPe = true;
          }
        }
        if (validPe) {
          tmpFile.moveFileTo(ytDlp);
        } else {
          tmpFile.deleteFile();
          LOG_ERROR("Rejected downloaded yt-dlp.exe.tmp: failed PE header or "
                    "minimum size verification.");
        }
      }
    }

    if (!ytDlp.existsAsFile()) {
      juce::MessageManager::callAsync([job]() {
        if (job.onComplete)
          job.onComplete(false, juce::File(), "",
                         "Could not locate or download yt-dlp.exe tool");
      });
      return;
    }

    setStatus("Fetching video audio (MP3 192k)...", 0.25f, job);

    // Build CLI command
    // yt-dlp -x --audio-format mp3 --audio-quality 192k --no-playlist
    // --ffmpeg-location <dir> -o "<dlDir>/%(title)s.%(ext)s" "<url>"
    juce::StringArray args;
    args.add(ytDlp.getFullPathName());
    args.add("-x");
    args.add("--audio-format");
    args.add("mp3");
    args.add("--audio-quality");
    args.add("192k");
    args.add("--no-playlist");
    args.add("--no-warnings");

    if (ffmpeg.existsAsFile()) {
      args.add("--ffmpeg-location");
      args.add(ffmpeg.getParentDirectory().getFullPathName());
    }

    juce::String outTemplate = dlDir.getFullPathName() + "/%(title)s.%(ext)s";
    args.add("-o");
    args.add(outTemplate);

    args.add("--"); // end of options: URL is a positional only
    args.add(job.url);

    std::map<juce::String, juce::Time> preExistingMp3s;
    for (const auto &f : dlDir.findChildFiles(juce::File::findFiles, false, "*.mp3"))
      preExistingMp3s[f.getFullPathName()] = f.getLastModificationTime();

    juce::ChildProcess process;
    if (!process.start(args, juce::ChildProcess::wantStdOut |
                                 juce::ChildProcess::wantStdErr)) {
      juce::MessageManager::callAsync([job]() {
        if (job.onComplete)
          job.onComplete(false, juce::File(), "",
                         "Failed to launch yt-dlp process");
      });
      return;
    }

    juce::String outputLog;
    juce::String detectedTitle;
    juce::File detectedFile;

    auto consumeOutputChunk = [&](const juce::String &chunk) {
      outputLog += chunk;
      if (chunk.contains("%")) {
        int pctIdx = chunk.indexOf("%");
        int startIdx = juce::jmax(0, pctIdx - 5);
        juce::String sub = chunk.substring(startIdx, pctIdx).trim();
        float pct = (float)sub.getDoubleValue();
        if (pct > 0.0f) {
          setStatus("Downloading: " + juce::String((int)pct) + "%",
                    0.3f + (pct * 0.005f), job);
        }
      }
      if (chunk.contains("[ExtractAudio]")) {
        setStatus("Converting to MP3 192k...", 0.90f, job);
      }
    };

    // Monitor process output
    bool cancelled = false;
    while (process.isRunning()) {
      if (threadShouldExit()) {
        cancelled = true;
        process.kill();
        process.waitForProcessToFinish(1000);
        break;
      }
      char buffer[512];
      int bytesRead = process.readProcessOutput(buffer, sizeof(buffer) - 1);
      if (bytesRead > 0) {
        buffer[bytesRead] = '\0';
        consumeOutputChunk(juce::String(buffer));
      }
      wait(50);
    }

    if (!cancelled) {
      for (;;) {
        char buffer[512];
        int bytesRead = process.readProcessOutput(buffer, sizeof(buffer) - 1);
        if (bytesRead <= 0)
          break;
        buffer[bytesRead] = '\0';
        consumeOutputChunk(juce::String(buffer));
      }
    }

    int exitCode = cancelled ? -1 : process.getExitCode();

    // Scan target directory for the newly created or updated mp3 file
    auto files = dlDir.findChildFiles(juce::File::findFiles, false, "*.mp3");
    juce::File newestNewFile;
    juce::Time newestNewTime(0);
    juce::File newestAnyFile;
    juce::Time newestAnyTime(0);

    for (const auto &f : files) {
      auto t = f.getLastModificationTime();
      const auto path = f.getFullPathName();
      auto it = preExistingMp3s.find(path);
      if (it == preExistingMp3s.end() || t > it->second) {
        if (t > newestNewTime) {
          newestNewTime = t;
          newestNewFile = f;
        }
      }
      if (t > newestAnyTime) {
        newestAnyTime = t;
        newestAnyFile = f;
      }
    }
    juce::File newestFile =
        newestNewFile.existsAsFile() ? newestNewFile : juce::File();

    bool success = (!cancelled && exitCode == 0 && newestFile.existsAsFile());
    if (success) {
      detectedFile = newestFile;
      detectedTitle = newestFile.getFileNameWithoutExtension();
      setStatus("Download complete: " + detectedTitle, 1.0f, job);
    } else {
      setStatus(cancelled
                    ? "Download cancelled"
                    : "Download failed (Exit code " + juce::String(exitCode) + ")",
                0.0f, job);
    }

    const int tailStart = juce::jmax(0, outputLog.length() - 300);
    const juce::String errTail =
        success ? juce::String() : outputLog.substring(tailStart);

    juce::MessageManager::callAsync(
        [job, success, detectedFile, detectedTitle, errTail]() {
          if (job.onComplete) {
            job.onComplete(success, detectedFile, detectedTitle, errTail);
          }
        });
  }

  mutable juce::CriticalSection configLock;
  juce::File customDownloadsDir;

  mutable juce::CriticalSection queueLock;
  std::queue<DownloadJob> jobQueue;

  mutable juce::CriticalSection statusLock;
  juce::String currentStatusText;
  std::atomic<bool> activeDownload{false};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(YoutubeDownloadManager)
};
