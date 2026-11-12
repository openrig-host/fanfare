#include "PluginMirrorCapture.h"

#if JUCE_WINDOWS
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #ifndef PW_CLIENTONLY
    #define PW_CLIENTONLY 0x00000001
  #endif
  #ifndef PW_RENDERFULLCONTENT
    #define PW_RENDERFULLCONTENT 0x00000002
  #endif
#endif

#if JUCE_MAC
  #include <CoreGraphics/CoreGraphics.h>
  #include <ApplicationServices/ApplicationServices.h>
#endif

#include <cstring>

namespace OpenRig {

namespace {
    constexpr float kMinCaptureScale = 0.25f;
    constexpr float kMaxCaptureScale = 4.0f;
}

// =====================================================================
// Target management
// =====================================================================

void PluginMirrorCapture::setTarget(const Target& newTarget) {
    std::lock_guard<std::mutex> lock(targetMutex);
    target = newTarget;
    if (target.captureScale < kMinCaptureScale || target.captureScale > kMaxCaptureScale)
        target.captureScale = 1.0f;
    updateFrameInfoLocked();
}

void PluginMirrorCapture::clearTarget() {
    std::lock_guard<std::mutex> lock(targetMutex);
    target = Target();
    lastFrameInfo = FrameInfo();
}

bool PluginMirrorCapture::hasTarget() const {
    std::lock_guard<std::mutex> lock(targetMutex);
    return target.isValid();
}

PluginMirrorCapture::Target PluginMirrorCapture::getTarget() const {
    std::lock_guard<std::mutex> lock(targetMutex);
    return target;
}

PluginMirrorCapture::FrameInfo PluginMirrorCapture::getFrameInfo() const {
    std::lock_guard<std::mutex> lock(targetMutex);
    return lastFrameInfo;
}

// =====================================================================
// Geometry resolution (frame bitmap <-> target input coordinates)
// =====================================================================

void PluginMirrorCapture::updateFrameInfoLocked() {
    lastFrameInfo = FrameInfo();
    if (!target.isValid())
        return;

    int inputW = 0, inputH = 0;
    juce::Point<int> inputOrigin;

    if (target.kind == Target::Kind::NativeWindow) {
        lastFrameInfo.inputWindowHandle = target.nativeWindowHandle;
    #if JUCE_WINDOWS
        RECT clientRect {};
        if (::GetClientRect((HWND)target.nativeWindowHandle, &clientRect) == FALSE)
            return;
        inputW = clientRect.right - clientRect.left;
        inputH = clientRect.bottom - clientRect.top;
    #elif JUCE_MAC
        // Prototype: assume the window client size matches the component size
        // captured via CGWindowListCreateImage with kCGWindowImageBoundsIgnoreFraming.
        // Production code resolves this via CGWindowListCopyWindowInfo.
        if (target.component != nullptr) {
            inputW = target.component->getWidth();
            inputH = target.component->getHeight();
        }
    #endif
    } else if (target.kind == Target::Kind::JuceComponent) {
        auto* comp = target.component.getComponent();
        if (comp == nullptr)
            return;
        auto* peer = comp->getPeer();
        if (peer == nullptr)
            return;
        lastFrameInfo.inputWindowHandle = peer->getNativeHandle();

        // Convert the component's logical bounds into the native client
        // coordinates of its top-level window: this is exactly the space the
        // synthetic mouse messages must use, and it absorbs per-monitor DPI
        // scaling (peer scale factor) in one place.
        const float scale = (float)peer->getPlatformScaleFactor();
        auto& topLevel = peer->getComponent();
        const juce::Point<int> offset = comp->getScreenPosition() - topLevel.getScreenPosition();
        inputOrigin = { juce::roundToInt(offset.x * scale), juce::roundToInt(offset.y * scale) };
        inputW = juce::roundToInt(comp->getWidth() * scale);
        inputH = juce::roundToInt(comp->getHeight() * scale);
    }

    if (inputW <= 0 || inputH <= 0)
        return;

    lastFrameInfo.inputRegion = juce::Rectangle<int>(inputOrigin.x, inputOrigin.y, inputW, inputH);
    lastFrameInfo.frameWidth  = juce::roundToInt(inputW * target.captureScale);
    lastFrameInfo.frameHeight = juce::roundToInt(inputH * target.captureScale);
}

// =====================================================================
// Frame capture (message thread)
// =====================================================================

juce::Image PluginMirrorCapture::captureFrame() {
    std::lock_guard<std::mutex> lock(targetMutex);
    if (!target.isValid())
        return {};

    juce::Image image;
    if (target.kind == Target::Kind::JuceComponent)
        image = captureJuceComponent();
    else
        image = captureNativeWindow();

    if (image.isNull())
        return {};

    // Native captures are grabbed at 1:1 device pixels; honour captureScale
    // (supersampling for hi-res tablets) so the bitmap size always matches the
    // geometry contract in FrameInfo.
    if (target.kind == Target::Kind::NativeWindow && target.captureScale != 1.0f) {
        const int scaledW = juce::roundToInt(image.getWidth() * target.captureScale);
        const int scaledH = juce::roundToInt(image.getHeight() * target.captureScale);
        if (scaledW > 0 && scaledH > 0)
            image = image.rescaled(scaledW, scaledH, juce::Graphics::highResamplingQuality);
    }

    updateFrameInfoLocked();
    return image;
}

juce::Image PluginMirrorCapture::captureJuceComponent() {
    // Note: the brief refers to juce::createComponentSnapshotImage(); the real
    // JUCE API is Component::createComponentSnapshot(), which renders the
    // component tree into a juce::Image. It must be called on the message
    // thread (the PluginMirrorStreamer capture timer guarantees this).
    auto* comp = target.component.getComponent();
    if (comp == nullptr)
        return {};

    return comp->createComponentSnapshot(comp->getLocalBounds(), target.captureScale);
}

juce::Image PluginMirrorCapture::captureNativeWindow() {
#if JUCE_WINDOWS
    return captureWin32Window();
#elif JUCE_MAC
    return captureMacWindow();
#else
    return {};
#endif
}

// =====================================================================
// Native window capture - Windows (PrintWindow / BitBlt)
// =====================================================================

#if JUCE_WINDOWS

namespace {
    bool isBitmapMostlyBlack(const uint32_t* pixels, int w, int h) noexcept {
        // Sparse probe (64 samples) to detect PrintWindow silently producing a
        // black surface for GPU-composed content.
        if (pixels == nullptr || w <= 0 || h <= 0)
            return true;
        const int samples = 64;
        const int step = juce::jmax(1, (w * h) / samples);
        for (int i = 0; i < w * h; i += step) {
            if ((pixels[i] & 0x00FFFFFF) != 0)
                return false;
        }
        return true;
    }
}

juce::Image PluginMirrorCapture::captureWin32Window() {
    HWND hwnd = (HWND)target.nativeWindowHandle;
    if (hwnd == nullptr || ::IsWindow(hwnd) == FALSE)
        return {};

    RECT clientRect {};
    if (::GetClientRect(hwnd, &clientRect) == FALSE)
        return {};
    const int w = clientRect.right - clientRect.left;
    const int h = clientRect.bottom - clientRect.top;
    if (w <= 0 || h <= 0)
        return {};

    HDC windowDC = ::GetDC(hwnd);
    if (windowDC == nullptr)
        return {};
    HDC memDC = ::CreateCompatibleDC(windowDC);
    if (memDC == nullptr) {
        ::ReleaseDC(hwnd, windowDC);
        return {};
    }

    BITMAPINFOHEADER bi {};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = -h; // negative => top-down rows, matching juce::Image layout
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(memDC, (const BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        ::DeleteDC(memDC);
        ::ReleaseDC(hwnd, windowDC);
        return {};
    }

    HGDIOBJ previous = ::SelectObject(memDC, dib);

    // PW_CLIENTONLY keeps the bitmap aligned with GetClientRect (the input
    // coordinate space); PW_RENDERFULLCONTENT includes DWM/DirectComposition
    // content that a plain BitBlt of the window DC misses.
    BOOL painted = ::PrintWindow(hwnd, memDC, PW_CLIENTONLY | PW_RENDERFULLCONTENT);

    // Fallback ladder for GPU-accelerated editors (see header notes on the
    // Windows.Graphics.Capture API for the production path).
    if (painted == FALSE || isBitmapMostlyBlack((const uint32_t*)bits, w, h))
        painted = ::BitBlt(memDC, 0, 0, w, h, windowDC, 0, 0, SRCCOPY);

    juce::Image image;
    if (painted != FALSE) {
        image = juce::Image(juce::Image::ARGB, w, h, false);
        juce::Image::BitmapData bd(image, juce::Image::BitmapData::writeOnly);
        const size_t rowBytes = (size_t)w * 4; // 32bpp BI_RGB rows are DWORD aligned
        for (int y = 0; y < h; ++y)
            std::memcpy(bd.getLinePointer(y), (const uint8_t*)bits + (size_t)y * rowBytes, rowBytes);
    }

    ::SelectObject(memDC, previous);
    ::DeleteObject(dib);
    ::DeleteDC(memDC);
    ::ReleaseDC(hwnd, windowDC);
    return image;
}

#endif // JUCE_WINDOWS

// =====================================================================
// Native window capture - macOS (CGWindowListCreateImage outline)
// =====================================================================

#if JUCE_MAC

juce::Image PluginMirrorCapture::captureMacWindow() {
    // PROTOTYPE PATH. CGWindowListCreateImage is deprecated since macOS 14 and
    // cannot capture GPU-composited content reliably; production should use
    // ScreenCaptureKit (SCStream with an SCContentFilter for this window id)
    // and convert each CVPixelBuffer row into a juce::Image.
    const CGWindowID windowId = (CGWindowID)(uintptr_t)target.nativeWindowHandle;
    CGImageRef cgImage = CGWindowListCreateImage(CGRectNull,
                                                 kCGWindowListOptionIncludingWindow,
                                                 windowId,
                                                 kCGWindowImageBoundsIgnoreFraming |
                                                 kCGWindowImageBestResolution);
    if (cgImage == nullptr)
        return {};

    const int w = (int)CGImageGetWidth(cgImage);
    const int h = (int)CGImageGetHeight(cgImage);
    juce::Image image;
    if (w > 0 && h > 0) {
        image = juce::Image(juce::Image::ARGB, w, h, false);
        juce::Image::BitmapData bd(image, juce::Image::BitmapData::writeOnly);

        // BGRA byte order (32-little + premultiplied first) matches the memory
        // layout of juce::Image::ARGB on little-endian targets.
        CGColorSpaceRef colourSpace = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(bd.data, (size_t)w, (size_t)h, 8,
                                                 (size_t)bd.lineStride, colourSpace,
                                                 kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
        if (ctx != nullptr) {
            CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cgImage);
            CGContextRelease(ctx);
        }
        CGColorSpaceRelease(colourSpace);
    }
    CGImageRelease(cgImage);
    return image;
}

#endif // JUCE_MAC

// =====================================================================
// JPEG compression (any thread)
// =====================================================================

std::vector<uint8_t> PluginMirrorCapture::encodeJpeg(const juce::Image& image, float quality) {
    if (image.isNull())
        return {};

    // JPEG has no alpha channel - drop it before encoding.
    const juce::Image rgb = image.convertedToFormat(juce::Image::RGB);

    juce::MemoryOutputStream stream;
    juce::JPEGImageFormat format;
    format.setQuality(juce::jlimit(0.05f, 1.0f, quality));
    if (!format.writeImageToStream(rgb, stream))
        return {};

    const auto& block = stream.getMemoryBlock();
    const auto* data = static_cast<const uint8_t*>(block.getData());
    return std::vector<uint8_t>(data, data + block.getSize());
}

} // namespace OpenRig
