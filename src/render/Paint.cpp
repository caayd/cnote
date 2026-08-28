#include "App.h"

#include "text/TextGeometry.h"

namespace textgeom = cnote::text;

namespace cnote {

namespace {

HBITMAP CreateTopDown32BitDibSection(HDC dc, int width, int height, void** bits) {
    if (bits == nullptr || width <= 0 || height <= 0) {
        return nullptr;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS, bits, nullptr, 0);
}

}  // namespace

void App::EnsurePaintBackbuffer(HDC referenceDc, int width, int height) {
    if (width <= 0 || height <= 0) {
        ReleasePaintBackbuffer();
        return;
    }

    if (paintBackbufferDc_ != nullptr
        && paintBackbufferBitmap_ != nullptr
        && paintBackbufferWidth_ == width
        && paintBackbufferHeight_ == height) {
        return;
    }

    ReleasePaintBackbuffer();

    paintBackbufferDc_ = CreateCompatibleDC(referenceDc);
    if (paintBackbufferDc_ == nullptr) {
        return;
    }

    void* bits = nullptr;
    paintBackbufferBitmap_ = CreateTopDown32BitDibSection(paintBackbufferDc_, width, height, &bits);
    if (paintBackbufferBitmap_ == nullptr || bits == nullptr) {
        ReleasePaintBackbuffer();
        return;
    }

    paintBackbufferPreviousBitmap_ = SelectObject(paintBackbufferDc_, paintBackbufferBitmap_);
    paintBackbufferWidth_ = width;
    paintBackbufferHeight_ = height;
}

void App::ReleasePaintBackbuffer() {
    if (paintBackbufferDc_ != nullptr && paintBackbufferPreviousBitmap_ != nullptr) {
        SelectObject(paintBackbufferDc_, paintBackbufferPreviousBitmap_);
    }
    paintBackbufferPreviousBitmap_ = nullptr;

    if (paintBackbufferBitmap_ != nullptr) {
        DeleteObject(paintBackbufferBitmap_);
        paintBackbufferBitmap_ = nullptr;
    }
    if (paintBackbufferDc_ != nullptr) {
        DeleteDC(paintBackbufferDc_);
        paintBackbufferDc_ = nullptr;
    }

    paintBackbufferWidth_ = 0;
    paintBackbufferHeight_ = 0;
}

void App::PaintWindowFallback(HDC windowDc, RECT clientRect, RECT paintRect, bool forceGdiOverlays) {
    EnsurePaintBackbuffer(windowDc, clientRect.right, clientRect.bottom);
    HDC memoryDc = paintBackbufferDc_ != nullptr ? paintBackbufferDc_ : windowDc;
    SaveDC(memoryDc);
    IntersectClipRect(memoryDc, paintRect.left, paintRect.top, paintRect.right, paintRect.bottom);

    HBRUSH backgroundBrush = CreateSolidBrush(RGB(18, 22, 29));
    FillRect(memoryDc, &paintRect, backgroundBrush);
    DeleteObject(backgroundBrush);

    PaintCanvas(memoryDc, clientRect, CanvasRect(clientRect));
    if (forceGdiOverlays || !UpdatePdfSidebarComposition(clientRect)) {
        PaintPdfSidebar(memoryDc, clientRect);
    }
    if (forceGdiOverlays || !UpdateToolbarComposition(clientRect)) {
        PaintToolbar(memoryDc, clientRect);
    }
    PaintQuickSizePopup(memoryDc, clientRect);
    PaintTextFormatPopup(memoryDc, clientRect);
    PaintPaletteMenu(memoryDc, clientRect);
    PaintPaletteWheel(memoryDc, clientRect);

    PaintLiveTextEditOverlay(memoryDc);

    if (memoryDc != windowDc) {
        BitBlt(
            windowDc,
            paintRect.left,
            paintRect.top,
            paintRect.right - paintRect.left,
            paintRect.bottom - paintRect.top,
            memoryDc,
            paintRect.left,
            paintRect.top,
            SRCCOPY);
    }

    RestoreDC(memoryDc, -1);
}

void App::WarmUpTextEditPresentation() {
    if (window_ == nullptr) {
        textEditPresentationWarmupPending_ = false;
        return;
    }
    if (IsWindowVisible(window_) == FALSE || IsIconic(window_) != FALSE) {
        return;
    }
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    if (clientRect.right <= clientRect.left || clientRect.bottom <= clientRect.top) {
        return;
    }
    textEditPresentationWarmupPending_ = false;

    // The text tool is the first and only feature that shows a child window,
    // paints the main window through GDI, and detaches the content visual.
    // Each transition has a one-time DWM/GDI setup cost (redirection surface
    // allocation, RichEdit module/class init, child window composition), and
    // paying all three on the first real text placement presented a blank
    // frame under the detached visual. Perform the whole transition here,
    // underneath the already-attached composition tree, where it is invisible.
    HWND warmupEdit = nullptr;
    if (textgeom::EnsureRichEditModule() != nullptr) {
        warmupEdit = CreateWindowExW(
            0,
            L"RICHEDIT50W",
            L"",
            WS_CHILD | ES_MULTILINE,
            0,
            0,
            1,
            1,
            window_,
            nullptr,
            instance_,
            nullptr);
        if (warmupEdit != nullptr) {
            ShowWindow(warmupEdit, SW_SHOW);
        }
    }

    HDC windowDc = GetDC(window_);
    if (windowDc != nullptr) {
        PaintWindowFallback(windowDc, clientRect, clientRect, true);
        ReleaseDC(window_, windowDc);
    }
    GdiFlush();

    // Detach and immediately reattach within the same handler: the fallback
    // above painted identical pixels, so no intermediate frame can differ.
    DetachContentComposition(false);
    if (warmupEdit != nullptr) {
        ShowWindow(warmupEdit, SW_HIDE);
        DestroyWindow(warmupEdit);
    }
    static_cast<void>(UpdateContentComposition(clientRect, clientRect));
}

void App::Paint() {
    PAINTSTRUCT paint {};
    HDC windowDc = BeginPaint(window_, &paint);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);

    RefreshTextEditControl();
    const bool contentComposited = UpdateContentComposition(clientRect, paint.rcPaint);
    if (contentComposited) {
        const bool sidebarComposited = UpdatePdfSidebarComposition(clientRect);
        const bool toolbarComposited = UpdateToolbarComposition(clientRect);
        const bool popupComposited = UpdatePopupComposition(clientRect);
        if (sidebarComposited && toolbarComposited && popupComposited) {
            EndPaint(window_, &paint);
            if (textEditPresentationWarmupPending_) {
                WarmUpTextEditPresentation();
            }
            return;
        }
    }

    const bool repaintFullFallback = contentCompositionContentAttached_;
    const RECT fallbackPaintRect = repaintFullFallback ? clientRect : paint.rcPaint;
    PaintWindowFallback(windowDc, clientRect, fallbackPaintRect, false);

    EndPaint(window_, &paint);
    DetachContentComposition(true);
    DetachPopupComposition(true);
    if (textEditPresentationWarmupPending_) {
        WarmUpTextEditPresentation();
    }
}

}  // namespace cnote
