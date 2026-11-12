#include "PluginMirrorInput.h"

#if JUCE_WINDOWS
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

#include <cmath>

namespace OpenRig {

// =====================================================================
// JSON parsing
// =====================================================================

bool PluginMirrorInputHandler::parseTouchMessage(const juce::String& jsonText, TouchEvent& out) {
    auto json = juce::JSON::parse(jsonText);
    if (!json.isObject())
        return false;
    if (json["type"].toString() != "plugin_touch")
        return false;

    const juce::String action = json["action"].toString().toLowerCase();
    if (action == "down")          out.action = TouchEvent::Action::Down;
    else if (action == "move")     out.action = TouchEvent::Action::Move;
    else if (action == "up")       out.action = TouchEvent::Action::Up;
    else return false;

    const auto xVar = json["x"];
    const auto yVar = json["y"];
    const bool xNumeric = xVar.isInt() || xVar.isInt64() || xVar.isDouble();
    const bool yNumeric = yVar.isInt() || yVar.isInt64() || yVar.isDouble();
    if (!xNumeric || !yNumeric)
        return false;

    out.x = (float)(double)xVar;
    out.y = (float)(double)yVar;
    if (!std::isfinite(out.x) || !std::isfinite(out.y))
        return false;

    out.pointerId = 0;
    if (json.hasProperty("pointerId")) {
        const auto idVar = json["pointerId"];
        if (idVar.isInt() || idVar.isInt64())
            out.pointerId = (int)idVar;
    }
    return true;
}

// =====================================================================
// Translation + injection
// =====================================================================

bool PluginMirrorInputHandler::inject(const TouchEvent& evt, const PluginMirrorCapture::FrameInfo& frameInfo) {
    const auto region = frameInfo.inputRegion;
    if (region.isEmpty() || frameInfo.frameWidth <= 0 || frameInfo.frameHeight <= 0)
        return false;

    // Mirror frame pixels -> target input coordinates (native client space).
    // Frame and region sizes differ when the capture is supersampled
    // (captureScale != 1) or the target uses per-monitor DPI scaling.
    const float nx = juce::jlimit(0.0f, 1.0f, evt.x / (float)frameInfo.frameWidth);
    const float ny = juce::jlimit(0.0f, 1.0f, evt.y / (float)frameInfo.frameHeight);
    const int clientX = juce::jlimit(region.getX(), region.getRight() - 1,
                                     region.getX() + juce::roundToInt(nx * region.getWidth()));
    const int clientY = juce::jlimit(region.getY(), region.getBottom() - 1,
                                     region.getY() + juce::roundToInt(ny * region.getHeight()));

    bool buttonDown = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto& down = pointerDownState[evt.pointerId];
        if (evt.action == TouchEvent::Action::Down) down = true;
        if (evt.action == TouchEvent::Action::Up)   down = false;
        buttonDown = down;
    }

    return injectNative(frameInfo.inputWindowHandle, clientX, clientY, evt.action, buttonDown);
}

bool PluginMirrorInputHandler::injectNative(void* windowHandle, int clientX, int clientY,
                                            TouchEvent::Action action, bool buttonDown) {
#if JUCE_WINDOWS
    HWND hwnd = (HWND)windowHandle;
    if (hwnd == nullptr || ::IsWindow(hwnd) == FALSE)
        return false;

    // Client-relative coordinates, exactly the space WM_LBUTTON* expects.
    const LPARAM lp = MAKELPARAM((WORD)juce::jmax(0, clientX), (WORD)juce::jmax(0, clientY));

    switch (action) {
        case TouchEvent::Action::Down:
            // Focus the editor first so its own mouse handling (capture, drags)
            // behaves like a real click.
            ::SetFocus(hwnd);
            ::PostMessage(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
            break;
        case TouchEvent::Action::Move:
            ::PostMessage(hwnd, WM_MOUSEMOVE, buttonDown ? MK_LBUTTON : 0, lp);
            break;
        case TouchEvent::Action::Up:
            ::PostMessage(hwnd, WM_LBUTTONUP, 0, lp);
            break;
    }

    // PRODUCTION NOTES:
    //  - Multi-touch: map additional pointerIds to WM_POINTERDOWN/WM_POINTERUP
    //    (or WM_TOUCH with RegisterTouchWindow) instead of the single left
    //    button synthesised here.
    //  - Right click / hover: WM_RBUTTONDOWN and WM_MOUSEMOVE without buttons.
    //  - Drag precision: PostMessage queues one message per client event; a
    //    coalescing Move (keeping only the latest per tick) reduces message
    //    storms from 120 Hz touch panels.
    return true;
#elif JUCE_MAC
    // OUTLINE (requires an Objective-C++ translation unit for NSWindow access):
    //   1. Resolve the window's global origin:
    //        NSWindow* win = [(NSView*)windowHandle window];
    //        NSRect frame = [win frame];  // global top-left, y-axis flipped
    //      globalX = frame.origin.x + clientX;
    //      globalY = frame.origin.y + frame.size.height - clientY;  // flip Y
    //   2. Create and post the event to this process only (no global cursor
    //      movement):
    //        CGEventRef ev = CGEventCreateMouseEvent(nullptr, type,
    //            CGPointMake(globalX, globalY), kCGMouseButtonLeft);
    //        CGEventPostToPid(getpid(), ev);
    //        CGEventRelease(ev);
    //      with type = kCGEventLeftMouseDown / kCGEventLeftMouseDragged /
    //                  kCGEventLeftMouseUp (dragged when buttonDown).
    juce::ignoreUnused(windowHandle, clientX, clientY, action, buttonDown);
    return false;
#else
    juce::ignoreUnused(windowHandle, clientX, clientY, action, buttonDown);
    return false;
#endif
}

} // namespace OpenRig
