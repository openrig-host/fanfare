#pragma once

#include <JuceHeader.h>
#include "PluginMirrorCapture.h"
#include <map>
#include <mutex>

namespace OpenRig {

/**
 * Reverse Touch Input Handler for the Live Plugin Mirror.
 *
 * Receives JSON touch events from tablet clients over the WebSocket, maps the
 * mirror-frame coordinates into the captured target's input coordinate space
 * (see PluginMirrorCapture::FrameInfo) and injects them as synthetic OS mouse
 * events:
 *
 *   Windows : PostMessage(WM_LBUTTONDOWN / WM_LBUTTONUP / WM_MOUSEMOVE) to the
 *             target HWND - exact, per-message, no global cursor movement and
 *             no UIPI interference for same-integrity targets. Runs safely on
 *             the WebSocket worker thread (PostMessage is thread-safe).
 *   macOS   : outline below via CGEventCreateMouseEvent + CGEventPostToPid.
 *
 * Wire format (matches the Stage HUD JSON envelope):
 *   {"type": "plugin_touch", "action": "down", "x": 420, "y": 180}
 *   {"type": "plugin_touch", "action": "move", "x": 424, "y": 182, "pointerId": 0}
 *   {"type": "plugin_touch", "action": "up",   "x": 424, "y": 182}
 *
 * x/y are mirror-frame pixels (the bitmap size announced in `mirror_info`);
 * clients scale their CSS/canvas coordinates into this space before sending.
 */
class PluginMirrorInputHandler {
public:
    PluginMirrorInputHandler() = default;

    struct TouchEvent {
        enum class Action { Down, Move, Up };

        Action action = Action::Down;
        float x = 0.0f; // mirror frame pixels
        float y = 0.0f;
        int pointerId = 0;
    };

    /** Parses one plugin_touch JSON message. Returns false for malformed input. */
    static bool parseTouchMessage(const juce::String& jsonText, TouchEvent& out);

    /** Translates the event into target coordinates and injects it.
        Safe to call from any thread. */
    bool inject(const TouchEvent& evt, const PluginMirrorCapture::FrameInfo& frameInfo);

private:
    bool injectNative(void* windowHandle, int clientX, int clientY, TouchEvent::Action action, bool buttonDown);

    std::mutex stateMutex;
    std::map<int, bool> pointerDownState; // pointerId -> button currently down

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginMirrorInputHandler)
};

} // namespace OpenRig
