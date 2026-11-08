/*
  ==============================================================================

    This file contains the basic startup code for a JUCE application.

  ==============================================================================
*/

#include "FanfareSplashScreen.h"
#include "MainComponent.h"
#include "Version.h"
#include <JuceHeader.h>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#pragma comment(lib, "winmm.lib")
#endif

//==============================================================================
class FanfareApplication : public juce::JUCEApplication {
public:
  //==============================================================================
  FanfareApplication() {}

  const juce::String getApplicationName() override {
    return "Fanfare";
  }
  const juce::String getApplicationVersion() override {
    return OPENRIG_VERSION_STRING;
  }
  bool moreThanOneInstanceAllowed() override { return true; }

  //==============================================================================
  void initialise(const juce::String &commandLine) override {
    juce::ignoreUnused(commandLine);

#ifdef _WIN32
    // Set Windows multimedia timer to 1ms resolution for accurate audio timing
    timeBeginPeriod(1);
#endif

    juce::SystemStats::setApplicationCrashHandler(FanfareLog::crashHandler);
#ifdef _WIN32
    // Must be called AFTER setApplicationCrashHandler so that fanfareUnhandledExceptionFilter
    // is the active Win32 top-level exception filter (preserving background thread isolation).
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
    mainWindow = nullptr; // (deletes our window)
    SaveHistory::waitForPendingSnapshots(2500);

#ifdef _WIN32
    // Restore default Windows timer resolution
    timeEndPeriod(1);
    // Force terminate process to prevent zombie VST threads from locking ASIO/MIDI drivers
    TerminateProcess(GetCurrentProcess(), 0);
#endif
  }

  //==============================================================================
  void systemRequestedQuit() override {
    // This is called when the app is being asked to quit: you can ignore this
    // request and let the app carry on running, or call quit() to allow the app
    // to close.
    quit();
  }

  void anotherInstanceStarted(const juce::String &commandLine) override {
    juce::ignoreUnused(commandLine);
    // When another instance of the app is launched while this one is running,
    // this method is invoked, and the commandLine parameter tells you what
    // the other instance's command-line arguments were.
  }

  //==============================================================================
  /*
      This class implements the desktop window that contains an instance of
      our MainComponent class.
  */
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

      // Reserve vertical headroom for the native OS title bar + frame border
      // before the peer exists so AdjustWindowRectEx never pushes top < 0.
      const int maxClientW = juce::jmax(960, workArea.getWidth() - 16);
      const int maxClientH = juce::jmax(560, workArea.getHeight() - 48);
      int initW = juce::jmin(getWidth(), maxClientW);
      int initH = juce::jmin(getHeight(), maxClientH);

      bool restoredSavedBounds = false;
      bool wantMaximised = false;
      auto settingsFile =
          FanfareConstants::getAppDirectory().getChildFile("ui_settings.json");
      if (settingsFile.existsAsFile()) {
        auto v = juce::JSON::parse(settingsFile.loadFileAsString());
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
        // Shift client area down slightly to account for native top title bar
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
      // This is called when the user tries to close this window. Here, we'll
      // just ask the app to quit when this happens, but you can change this to
      // do whatever you need.
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

            if (winW > workW)
              winW = workW;
            if (winH > workH)
              winH = workH;

            if (winX + winW > mi.rcWork.right)
              winX = mi.rcWork.right - winW;
            if (winX < mi.rcWork.left)
              winX = mi.rcWork.left;

            if (winY + winH > mi.rcWork.bottom)
              winY = mi.rcWork.bottom - winH;
            if (winY < mi.rcWork.top)
              winY = mi.rcWork.top;

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
      auto f =
          FanfareConstants::getAppDirectory().getChildFile("ui_settings.json");
      juce::var v;
      if (f.existsAsFile()) {
        const auto raw = f.loadFileAsString().trim();
        if (raw.isNotEmpty()) {
          v = juce::JSON::parse(raw);
          if (!v.isObject())
            return; // Corrupt ui_settings.json — abort rather than wiping unrelated settings
        }
      }
      if (!v.isObject())
        v = juce::var(new juce::DynamicObject());
      if (auto *obj = v.getDynamicObject()) {
        const bool maximised = isFullScreen();
        obj->setProperty("windowMaximised", maximised);
        if (!maximised && !isMinimised() && getWidth() >= 960 &&
            getHeight() >= 560) {
          obj->setProperty("windowBounds", getBounds().toString());
        }
      }
      f.getParentDirectory().createDirectory();
      juce::TemporaryFile tmp(f);
      if (tmp.getFile().replaceWithText(juce::JSON::toString(v)))
        tmp.overwriteTargetFileWithTemporary();
#endif
    }

    /* Note: Be careful if you override any DocumentWindow methods - the base
       class uses a lot of them, so by overriding you might break its
       functionality. It's best to do all your work in your content component
       instead, but if you really have to override any DocumentWindow methods,
       make sure your subclass also calls the superclass's method.
    */

  private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
  };

private:
  std::unique_ptr<Fanfare::FanfareSplashScreen> splashScreen;
  std::unique_ptr<MainWindow> mainWindow;
};

//==============================================================================
// Custom entry point with SEH-protected message dispatch.
//
// Plugins like Kontakt and Super 8 create internal hidden windows during
// initialization. When the JUCE message pump dispatches Windows messages to
// these windows, the plugin's window procedure can access-violate (null
// pointer). Standard C++ try/catch cannot catch access violations (0xC0000005)
// because they are SEH (structured) exceptions, not C++ exceptions.
//
// By wrapping runDispatchLoopUntil() in __try/__except, the access violation
// is caught and the stack unwound to a safe point. The app survives; the
// plugin may be unstable but the host keeps running. This is critical for
// live performance reliability.

#ifdef _WIN32
// Must be a separate function: MSVC C2712 forbids __try in functions that
// contain C++ objects with destructors.
static void runDispatchLoopSEH() {
  __try {
    juce::MessageManager::getInstance()->runDispatchLoop();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Plugin access violation in message dispatch — swallowed.
  }
}
#endif

// Factory for the application instance.
static juce::JUCEApplicationBase *juce_CreateApplication() {
  return new FanfareApplication();
}

#ifdef _WIN32
extern "C" int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
  juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;

  juce::ScopedJuceInitialiser_GUI libraryInitialiser;
  auto app = std::unique_ptr<juce::JUCEApplicationBase>(
      juce::JUCEApplicationBase::createInstance());
  jassert(app != nullptr);

  app->initialise(juce::String(lpCmdLine));

  // SEH-protected message loop: each plugin window crash is caught and
  // contained. The loop continues processing subsequent messages.
  while (!juce::MessageManager::getInstance()->hasStopMessageBeenSent())
    runDispatchLoopSEH();

  app->shutdown();
  return 0;
}
#else
START_JUCE_APPLICATION(FanfareApplication)
#endif
