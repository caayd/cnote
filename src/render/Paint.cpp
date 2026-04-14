#include "App.h"

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

void App::Paint() {
    PAINTSTRUCT paint {};
    HDC windowDc = BeginPaint(window_, &paint);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);

    EnsurePaintBackbuffer(windowDc, clientRect.right, clientRect.bottom);
    HDC memoryDc = paintBackbufferDc_ != nullptr ? paintBackbufferDc_ : windowDc;
    SaveDC(memoryDc);
    IntersectClipRect(memoryDc, paint.rcPaint.left, paint.rcPaint.top, paint.rcPaint.right, paint.rcPaint.bottom);

    HBRUSH backgroundBrush = CreateSolidBrush(RGB(18, 22, 29));
    FillRect(memoryDc, &paint.rcPaint, backgroundBrush);
    DeleteObject(backgroundBrush);

    RefreshTextEditControl();
    PaintCanvas(memoryDc, clientRect);
    PaintToolbar(memoryDc, clientRect);
    PaintQuickSizePopup(memoryDc, clientRect);
    PaintTextFormatPopup(memoryDc, clientRect);
    PaintPaletteMenu(memoryDc, clientRect);
    PaintPaletteWheel(memoryDc, clientRect);

    PaintLiveTextEditOverlay(memoryDc);

    if (memoryDc != windowDc) {
        BitBlt(
            windowDc,
            paint.rcPaint.left,
            paint.rcPaint.top,
            paint.rcPaint.right - paint.rcPaint.left,
            paint.rcPaint.bottom - paint.rcPaint.top,
            memoryDc,
            paint.rcPaint.left,
            paint.rcPaint.top,
            SRCCOPY);
    }

    RestoreDC(memoryDc, -1);

    EndPaint(window_, &paint);
}

}  // namespace cnote
