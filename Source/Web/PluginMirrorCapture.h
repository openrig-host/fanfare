#pragma once

#include <JuceHeader.h>
#include <cstdint>
#include <mutex>
#include <vector>

namespace OpenRig {

/**
 * Frame Capture Engine for the Live Plugin Mirror.
 *
 * Captures the bitmap of a hosted plugin editor (or any native top-level window)
 * and compresses it to JPEG for the streaming layer. Two capture paths are
 * provided:
 *
 *  1. JUCE component capture via juce::Component::createComponentSnapshot() -
 *     zero-copy into a juce::Image, must run on the message thread.
 *  2. Native OS window capture - required for VST3 editors that host their own
 *     child HWNDs (which createComponentSnapshot cannot paint) and for content
 *     rendered by the plugin process itself:
 *       - Windows: PrintWindow() with PW_RENDERFULLCONTENT (falls back to
 *         BitBlt from the window DC).
 *       - macOS:   CGWindowListCreateImage() (prototype path; see notes on
 *         ScreenCaptureKit below).
 *
 * ENGINEERING HURDLE - GPU-accelerated windows: PrintWindow returns an empty or
 * black bitmap for surfaces composed on the GPU (DirectComposition/D3D/OpenGL
 * editors, Chromium-based UIs). Production strategy (Windows):
 *  - Use the Windows.Graphics.Capture API (WinRT):
 *      Direct3D11CaptureFramePool.CreateFreeThreaded() + GraphicsCaptureItem
 *      from the HWND, pick up frames as ID3D11Texture2D, blit into a staging
 *      texture, Map() and copy the BGRA rows straight into a juce::Image.
 *  - This captures GPU-composed content including occluded windows and works on
 *    Windows 10 1803+. The fallback ladder implemented below is:
 *      PrintWindow(PW_RENDERFULLCONTENT) -> validate frame is not fully black
 *      -> BitBlt from screen DC of the window rect (always works when visible).
 *  - On macOS, CGWindowListCreateImage is deprecated since 14.0; migrate to
 *    ScreenCaptureKit (SCStream + SCContentFilter for the window) which also
 *    handles GPU content and delivers frames on a serial dispatch queue.
 *
 * ENGINEERING HURDLE - DPI / coordinate scaling: see FrameInfo::inputRegion.
 * The capture always reports the bitmap size it produced plus the rectangle of
 * the target's input coordinate space (native client coordinates), so the
 * reverse-touch handler can map mirror pixels to exact mouse coordinates
 * regardless of per-monitor DPI scaling or supersampled capture.
 */
class PluginMirrorCapture {
public:
    /** What to capture: an embedded JUCE component (e.g. an AudioProcessorEditor)
        or a native OS window (HWND on Windows, CGWindowID on macOS). */
    struct Target {
        enum class Kind { None, JuceComponent, NativeWindow };

        Kind kind = Kind::None;
        juce::Component::SafePointer<juce::Component> component; // Kind::JuceComponent
        void* nativeWindowHandle = nullptr;                      // Kind::NativeWindow (HWND / CGWindowID)
        juce::String name;                                       // display name for status messages
        float captureScale = 1.0f;                               // >1.0 supersamples for hi-res tablets

        bool isValid() const noexcept {
            return kind == Kind::NativeWindow ? nativeWindowHandle != nullptr
                                              : (kind == Kind::JuceComponent && component != nullptr);
        }
    };

    /** Geometry contract between capture and input injection, resolved for both
        target kinds into one uniform form:
        - inputWindowHandle: native window whose client area receives synthetic
          mouse messages (HWND on Windows). For JUCE component targets this is
          the hosting top-level peer, with coordinates already converted to the
          physical pixels PostMessage expects (per-monitor DPI safe).
        - inputRegion: the mirrored region inside that client area.
        - frameWidth/frameHeight: bitmap pixels delivered to clients.
        frameX maps to inputRegion.x + frameX * inputRegion.width / frameWidth. */
    struct FrameInfo {
        int frameWidth = 0;
        int frameHeight = 0;
        juce::Rectangle<int> inputRegion;
        void* inputWindowHandle = nullptr;
    };

    PluginMirrorCapture() = default;

    // ---- Target management (message thread) ----
    void setTarget(const Target& newTarget);
    void clearTarget();
    bool hasTarget() const;
    Target getTarget() const;

    // ---- Frame capture (message thread; cheap snapshot, no encoding) ----
    juce::Image captureFrame();
    FrameInfo getFrameInfo() const;

    // ---- Compression (any thread) ----
    static std::vector<uint8_t> encodeJpeg(const juce::Image& image, float quality);

private:
    juce::Image captureJuceComponent();
    juce::Image captureNativeWindow();
    void updateFrameInfoLocked();

#if JUCE_WINDOWS
    juce::Image captureWin32Window();
#endif
#if JUCE_MAC
    juce::Image captureMacWindow();
#endif

    mutable std::mutex targetMutex;
    Target target;
    FrameInfo lastFrameInfo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginMirrorCapture)
};

} // namespace OpenRig
