#include "AppSettings.h"
#include "FanfareSplashScreen.h"
#include "MainComponent.h"
#include "Version.h"
#include <JuceHeader.h>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#pragma comment(lib, "winmm.lib")
#else
#include <csignal>
#endif

class FanfareApplication : public juce::JUCEApplication {
public:
  FanfareApplication() {}

  const juce::String getApplicationName() override { return "Fanfare"; }
  const juce::String getApplicationVersion() override { return OPENRIG_VERSION_STRING; }
  bool moreThanOneInstanceAllowed() override { return true; }

  void initialise(const juce::String &commandLine) override {
    juce::ignoreUnused(commandLine);

#ifdef _WIN32
    timeBeginPeriod(1);
#else
    // Prevent SIGPIPE from killing the host if a Tablet HUD client disconnects mid-frame
    std::signal(SIGPIPE, SIG_IGN);
#endif

    juce::SystemStats::setApplicationCrashHandler(FanfareLog::crashHandler);
#ifdef _WIN32
    FanfareLog::setupCrashHandlers();
#endif

    ThemeManager::getInstance().loadFromSettings();
    splashScreen = std::make_unique<Fanfare::FanfareSplashScreen>();
    splashScreen->setStage(0.16f, "Initializing stage engine...");

    mainWindow.reset(new MainWindow(getApplicationName() + " v" + getApplicationVersion() +
                                    "  [Build: " + juce::String(__DATE__) + " " + juce::String(__TIME__) + "]"));

    if (splashScreen) {
      splashScreen->onFinished = [this] {
        splashScreen.reset();
        if (mainWindow != nullptr) {
          if (auto *mc = dynamic_cast<MainComponent *>(mainWindow->getContentComponent()))
            mc->onStartupSplashFinished();
        }
      };
      splashScreen->finishAndDismiss();
    } else if (mainWindow != nullptr) {
      if (auto *mc = dynamic_cast<MainComponent *>(mainWindow->getContentComponent()))
        mc->onStartupSplashFinished();
    }
  }

  void shutdown() override {
    LOG_INFO("Fanfare shutting down cleanly. Flushing log and terminating process...");
    FanfareLog::flushLog();
    splashScreen = nullptr;
    mainWindow = nullptr;
    SaveHistory::waitForPendingSnapshots(2500);

#ifdef _WIN32
    timeEndPeriod(1);
    TerminateProcess(GetCurrentProcess(), 0);
#endif
  }

  void systemRequestedQuit() override { quit(); }
  void anotherInstanceStarted(const juce::String &commandLine) override { juce::ignoreUnused(commandLine); }

  class MainWindow : public juce::DocumentWindow {
  public:
    MainWindow(juce::String name)
        : DocumentWindow(
              name,
              juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                  juce::ResizableWindow::backgroundColourId),
              DocumentWindow::allButtons) {
      setUsingNativeTitleBar(true);
      setIcon(Fanfare::getAppIconImage());
      setContentOwned(new MainComponent(), true);

#if JUCE_IOS || JUCE_ANDROID
      setFullScreen(true);
#else
      setResizable(true, true);
      setResizeLimits(960, 560, 16384, 16384);

      const auto *display =
          juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
      const juce::Rectangle<int> workArea =
          display != nullptr ? display->userArea : getParentMonitorArea();

      const int maxClientW = juce::jmax(960, workArea.getWidth() - 16);
      const int maxClientH = juce::jmax(560, workArea.getHeight() - 48);
      int initW = juce::jmin(getWidth(), maxClientW);
      int initH = juce::jmin(getHeight(), maxClientH);

      bool restoredSavedBounds = false;
      bool wantMaximised = false;
      {
        const auto v = Fanfare::AppSettings::getInstance().getSnapshot();
        if (v.isObject()) {
          if (v.hasProperty("windowMaximised"))
            wantMaximised = (bool)v["windowMaximised"];
          juce::String savedBounds =
              v.getProperty("windowBounds", "").toString().trim();
          if (savedBounds.isNotEmpty()) {
            auto r = juce::Rectangle<int>::fromString(savedBounds);
            if (r.getWidth() >= 960 && r.getHeight() >= 560) {
              setBoundsConstrained(r);
              restoredSavedBounds = true;
            }
          }
        }
      }

      if (!restoredSavedBounds) {
        centreWithSize(initW, initH);
        setTopLeftPosition(getX(),
                           juce::jmax(workArea.getY() + 34, getY() + 14));
      }
#endif

      setVisible(true);
      if (auto *peer = getPeer())
        peer->setIcon(Fanfare::getAppIconImage());

#if !(JUCE_IOS || JUCE_ANDROID)
      if (wantMaximised) {
        setFullScreen(true);
      } else {
        clampNativeFrameToWorkArea();
        juce::Component::SafePointer<MainWindow> safeThis(this);
        juce::MessageManager::callAsync([safeThis]() {
          if (safeThis != nullptr && !safeThis->isFullScreen())
            safeThis->clampNativeFrameToWorkArea();
        });
      }
#endif
    }

    ~MainWindow() override { saveWindowBoundsToSettings(); }

    void closeButtonPressed() override {
      saveWindowBoundsToSettings();
      JUCEApplication::getInstance()->systemRequestedQuit();
    }

    void clampNativeFrameToWorkArea() {
#ifdef _WIN32
      if (auto *peer = getPeer()) {
        HWND hwnd = static_cast<HWND>(peer->getNativeHandle());
        if (hwnd != nullptr && !IsIconic(hwnd) && !IsZoomed(hwnd)) {
          HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
          MONITORINFO mi{};
          mi.cbSize = sizeof(mi);
          RECT wr{};
          if (GetMonitorInfo(hMon, &mi) && GetWindowRect(hwnd, &wr)) {
            const int workW = mi.rcWork.right - mi.rcWork.left;
            const int workH = mi.rcWork.bottom - mi.rcWork.top;
            int winW = wr.right - wr.left;
            int winH = wr.bottom - wr.top;
            int winX = wr.left;
            int winY = wr.top;

            if (winW > workW) winW = workW;
            if (winH > workH) winH = workH;
            if (winX + winW > mi.rcWork.right) winX = mi.rcWork.right - winW;
            if (winX < mi.rcWork.left) winX = mi.rcWork.left;
            if (winY + winH > mi.rcWork.bottom) winY = mi.rcWork.bottom - winH;
            if (winY < mi.rcWork.top) winY = mi.rcWork.top;

            if (winX != wr.left || winY != wr.top ||
                winW != (wr.right - wr.left) || winH != (wr.bottom - wr.top)) {
              SetWindowPos(hwnd, nullptr, winX, winY, winW, winH,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            }
          }
        }
      }
#endif
    }

    void saveWindowBoundsToSettings() const {
#if !(JUCE_IOS || JUCE_ANDROID)
      Fanfare::AppSettings::getInstance().update([this](juce::DynamicObject &obj) {
        const bool maximised = isFullScreen();
        obj.setProperty("windowMaximised", maximised);
        if (!maximised && !isMinimised() && getWidth() >= 960 &&
            getHeight() >= 560) {
          obj.setProperty("windowBounds", getBounds().toString());
        }
      });
#endif
    }

  private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
  };

private:
  std::unique_ptr<Fanfare::FanfareSplashScreen> splashScreen;
  std::unique_ptr<MainWindow> mainWindow;
};

#ifdef _WIN32
static void runDispatchLoopSEH() {
  __try {
    juce::MessageManager::getInstance()->runDispatchLoop();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

static juce::JUCEApplicationBase *juce_CreateApplication() {
  return new FanfareApplication();
}

extern "C" int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
  juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;

  juce::ScopedJuceInitialiser_GUI libraryInitialiser;
  auto app = std::unique_ptr<juce::JUCEApplicationBase>(
      juce::JUCEApplicationBase::createInstance());
  jassert(app != nullptr);

  app->initialise(juce::String(lpCmdLine));

  while (!juce::MessageManager::getInstance()->hasStopMessageBeenSent())
    runDispatchLoopSEH();

  app->shutdown();
  return 0;
}
#else
START_JUCE_APPLICATION(FanfareApplication)
#endif
