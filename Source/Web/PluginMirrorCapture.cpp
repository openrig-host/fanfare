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
  #include <AvailabilityMacros.h>
  #if defined(MAC_OS_X_VERSION_MAX_ALLOWED) && MAC_OS_X_VERSION_MAX_ALLOWED < 150000
    #include <CoreGraphics/CoreGraphics.h>
    #include <ApplicationServices/ApplicationServices.h>
  #endif
#endif

#include <cstring>

namespace OpenRig {

namespace {
    constexpr float kMinCaptureScale = 0.25f;
    constexpr float kMaxCaptureScale = 4.0f;
}

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

#if JUCE_WINDOWS

namespace {
    bool isBitmapMostlyBlack(const uint32_t* pixels, int w, int h) noexcept {
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
    bi.biHeight = -h;
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

    BOOL painted = ::PrintWindow(hwnd, memDC, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
    if (painted == FALSE || isBitmapMostlyBlack((const uint32_t*)bits, w, h))
        painted = ::BitBlt(memDC, 0, 0, w, h, windowDC, 0, 0, SRCCOPY);

    juce::Image image;
    if (painted != FALSE) {
        image = juce::Image(juce::Image::ARGB, w, h, false);
        juce::Image::BitmapData bd(image, juce::Image::BitmapData::writeOnly);
        const size_t rowBytes = (size_t)w * 4;
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

#if JUCE_MAC

juce::Image PluginMirrorCapture::captureMacWindow() {
#if defined(MAC_OS_X_VERSION_MAX_ALLOWED) && MAC_OS_X_VERSION_MAX_ALLOWED < 150000
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
#else
    // macOS 15+ SDK marks CGWindowListCreateImage obsolete; ScreenCaptureKit
    // capture will be wired here in Phase 4. Return null to fall back to
    // JuceComponent snapshot capture.
    return {};
#endif
}

#endif // JUCE_MAC

std::vector<uint8_t> PluginMirrorCapture::encodeJpeg(const juce::Image& image, float quality) {
    if (image.isNull())
        return {};

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
