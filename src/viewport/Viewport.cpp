#include "App.h"

#include <algorithm>
#include <cmath>

#include "DpiUtil.h"

namespace cnote {

namespace {

constexpr int kBaseToolbarHeight = 34;
constexpr int kMaximizedToolbarExtraHeight = 5;

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect { left, top, right, bottom };
    return rect;
}

int WindowFrameThicknessX(UINT dpi) {
    return ScaleForDpi(8, dpi);
}

int ToolbarHeight(bool maximized, UINT dpi) {
    return ScaleForDpi(kBaseToolbarHeight + (maximized ? kMaximizedToolbarExtraHeight : 0), dpi);
}

float DistanceSquaredToRect(const RECT& rect, POINT point) {
    const float clampedX = static_cast<float>(std::clamp(point.x, rect.left, rect.right - 1));
    const float clampedY = static_cast<float>(std::clamp(point.y, rect.top, rect.bottom - 1));
    const float dx = static_cast<float>(point.x) - clampedX;
    const float dy = static_cast<float>(point.y) - clampedY;
    return (dx * dx) + (dy * dy);
}

}  // namespace

void App::ResetView() {
    CancelToolbarZoomInput();
    zoom_ = 1.0f;
    targetZoom_ = 1.0f;
    panX_ = 0.0f;
    panY_ = 0.0f;
    if (window_ != nullptr) {
        RECT clientRect {};
        GetClientRect(window_, &clientRect);
        RECT toolbarRect = clientRect;
        toolbarRect.bottom = ToolbarHeight(IsZoomed(window_), WindowDpi(window_));
        InvalidateRect(window_, &toolbarRect, FALSE);
    }
}

void App::FocusPage(std::size_t index) {
    if (index >= document_.PageCount() || window_ == nullptr) {
        return;
    }

    document_.SetCurrentPageIndex(index);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, index);
    if (!pageView.valid) {
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    EnsureSidebarCurrentPageVisible();
    const int toolbarHeight = ToolbarHeight(IsZoomed(window_), WindowDpi(window_));
    const int canvasTop = toolbarHeight;
    const int canvasHeight = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top) - toolbarHeight);
    const int pageCenterY = (pageView.rect.top + pageView.rect.bottom) / 2;
    const int desiredCenterY = canvasTop + (canvasHeight / 2);
    panY_ += static_cast<float>(desiredCenterY - pageCenterY);
    InvalidateRect(window_, nullptr, FALSE);
}

RECT App::CanvasRect(RECT clientRect) const {
    RECT rect = clientRect;
    const UINT dpi = WindowDpi(window_);
    const int horizontalInset = IsZoomed(window_) ? WindowFrameThicknessX(dpi) : 0;
    rect.top = ToolbarHeight(IsZoomed(window_), dpi);
    rect.left += horizontalInset + PdfSidebarWidth();
    rect.right -= horizontalInset;
    return rect;
}

void App::PanBy(float dx, float dy) {
    panX_ += dx;
    panY_ += dy;
    ScheduleDeferredRasterRefresh();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::SetZoom(float zoom, std::optional<POINT> anchor) {
    if (window_ == nullptr) {
        zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
        targetZoom_ = zoom_;
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    POINT focusPoint {
        (clientRect.left + clientRect.right) / 2,
        (clientRect.top + clientRect.bottom) / 2,
    };
    if (anchor.has_value()) {
        focusPoint = *anchor;
    }

    const auto pageBefore = ResolveZoomAnchorPage(clientRect, focusPoint);
    const Vec2 anchorPagePoint = pageBefore.has_value()
        ? ScreenToPage(*pageBefore, focusPoint, false)
        : Vec2 {};

    const float clampedZoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    const float previousZoom = zoom_;
    targetZoom_ = clampedZoom;
    zoom_ = clampedZoom;
    if (std::abs(previousZoom - zoom_) < 0.001f) {
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    if (pageBefore.has_value() && previousZoom > 0.0f) {
        const POINT newScreenPoint = PageToScreen(pageBefore->pageIndex, anchorPagePoint);
        panX_ += static_cast<float>(focusPoint.x - newScreenPoint.x);
        panY_ += static_cast<float>(focusPoint.y - newScreenPoint.y);
    }

    zoomSessionActive_ = true;
    zoomAnchorValid_ = false;
    KillTimer(window_, kZoomSessionTimerId);
    SetTimer(window_, kZoomSessionTimerId, kZoomSessionDelayMs, nullptr);
    InvalidateAllPageSurfaceCaches();
    EnsureVisiblePageRasterQuality();
    InvalidateRect(window_, nullptr, FALSE);
}

bool App::IsCursorInsidePage(POINT cursor) const {
    return HitTestPage(cursor).has_value();
}

std::optional<App::PageView> App::HitTestPage(POINT cursor) const {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (pageView.valid && PtInRect(&pageView.rect, cursor) != 0) {
            return pageView;
        }
    }
    return std::nullopt;
}

std::optional<App::PageView> App::ResolveZoomAnchorPage(RECT clientRect, POINT anchorPoint) const {
    std::optional<PageView> bestPage;
    float bestDistanceSquared = 0.0f;
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }

        const float distanceSquared = DistanceSquaredToRect(pageView.rect, anchorPoint);
        if (!bestPage.has_value() || distanceSquared < bestDistanceSquared) {
            bestPage = pageView;
            bestDistanceSquared = distanceSquared;
            if (distanceSquared <= 0.0f) {
                break;
            }
        }
    }

    return bestPage;
}

Vec2 App::ScreenToPage(POINT cursor, std::size_t* pageIndex) const {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value() || hitPage->scale <= 0.0f) {
        if (pageIndex != nullptr) {
            *pageIndex = document_.CurrentPageIndex();
        }
        return {};
    }

    if (pageIndex != nullptr) {
        *pageIndex = hitPage->pageIndex;
    }

    const Page& page = document_.PageAt(hitPage->pageIndex);
    const float pageX = (static_cast<float>(cursor.x - hitPage->rect.left)) / hitPage->scale;
    const float pageY = (static_cast<float>(cursor.y - hitPage->rect.top)) / hitPage->scale;
    return {
        std::clamp(pageX, 0.0f, page.width),
        std::clamp(pageY, 0.0f, page.height),
    };
}

Vec2 App::ScreenToPage(const PageView& pageView, POINT cursor, bool clampToPage) const {
    if (!pageView.valid || pageView.pageIndex >= document_.PageCount() || pageView.scale <= 0.0f) {
        return {};
    }

    const Page& page = document_.PageAt(pageView.pageIndex);
    const float pageX = (static_cast<float>(cursor.x - pageView.rect.left)) / pageView.scale;
    const float pageY = (static_cast<float>(cursor.y - pageView.rect.top)) / pageView.scale;
    if (!clampToPage) {
        return { pageX, pageY };
    }

    return {
        std::clamp(pageX, 0.0f, page.width),
        std::clamp(pageY, 0.0f, page.height),
    };
}

POINT App::PageToScreen(std::size_t pageIndex, Vec2 point) const {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, pageIndex);
    POINT output {};
    output.x = pageView.rect.left + static_cast<LONG>(std::lround(point.x * pageView.scale));
    output.y = pageView.rect.top + static_cast<LONG>(std::lround(point.y * pageView.scale));
    return output;
}

App::PageView App::CalculatePageView(RECT clientRect, std::size_t pageIndex) const {
    const std::vector<PageView> pageViews = CalculatePageViews(clientRect);
    if (pageIndex >= pageViews.size()) {
        return {};
    }

    return pageViews[pageIndex];
}

std::vector<App::PageView> App::CalculatePageViews(RECT clientRect) const {
    const UINT dpi = WindowDpi(window_);
    const RECT canvasRect = CanvasRect(clientRect);
    const int canvasWidth = std::max(1, static_cast<int>(canvasRect.right - canvasRect.left));
    float widestPage = 1.0f;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        widestPage = std::max(widestPage, document_.PageAt(pageIndex).width);
    }

    const float fitScale = static_cast<float>(canvasWidth - ScaleForDpi(120, dpi)) / widestPage;
    const float scale = std::max(0.1f, fitScale * zoom_);
    const int canvasCenterX = canvasRect.left + (canvasWidth / 2);
    int top = canvasRect.top + ScaleForDpi(40, dpi) + static_cast<int>(std::lround(panY_));

    std::vector<PageView> pageViews;
    pageViews.reserve(document_.PageCount());
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        const Page& page = document_.PageAt(pageIndex);
        const int pageWidth = static_cast<int>(std::lround(page.width * scale));
        const int pageHeight = static_cast<int>(std::lround(page.height * scale));
        const int left = static_cast<int>(std::lround(canvasCenterX - (pageWidth / 2.0f) + panX_));
        pageViews.push_back({
            MakeRect(left, top, left + pageWidth, top + pageHeight),
            pageIndex,
            scale,
            pageWidth > 0 && pageHeight > 0,
        });
        top += pageHeight + ScaleForDpi(kPageGap, dpi);
    }

    return pageViews;
}

std::optional<std::size_t> App::ViewedPageIndex(RECT clientRect) const {
    if (document_.PageCount() == 0U) {
        return std::nullopt;
    }

    const RECT canvasRect = CanvasRect(clientRect);
    const int viewportCenterY = canvasRect.top + ((canvasRect.bottom - canvasRect.top) / 2);
    bool hasBestPage = false;
    std::size_t bestPageIndex = document_.CurrentPageIndex();
    int bestVisibleHeight = -1;
    int bestCenterDistance = 0;

    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }

        const int visibleTop = std::max(static_cast<int>(pageView.rect.top), static_cast<int>(canvasRect.top));
        const int visibleBottom = std::min(static_cast<int>(pageView.rect.bottom), static_cast<int>(canvasRect.bottom));
        const int visibleHeight = std::max(0, visibleBottom - visibleTop);
        const int pageCenterY = static_cast<int>((pageView.rect.top + pageView.rect.bottom) / 2);
        const int centerDistance = std::abs(pageCenterY - viewportCenterY);

        if (!hasBestPage
            || visibleHeight > bestVisibleHeight
            || (visibleHeight == bestVisibleHeight && centerDistance < bestCenterDistance)) {
            hasBestPage = true;
            bestPageIndex = pageView.pageIndex;
            bestVisibleHeight = visibleHeight;
            bestCenterDistance = centerDistance;
        }
    }

    return hasBestPage ? std::optional<std::size_t>(bestPageIndex) : std::nullopt;
}

bool App::ApplyZoomPreset(std::size_t presetIndex) {
    if (presetIndex >= settings_.zoomPresets.size()) {
        return false;
    }
    const float presetZoom = settings_.zoomPresets[presetIndex];
    if (presetZoom <= 0.0f) {
        return false;
    }

    const float clampedZoom = std::clamp(presetZoom, kMinZoom, kMaxZoom);
    if (window_ == nullptr) {
        zoom_ = clampedZoom;
        targetZoom_ = clampedZoom;
        return true;
    }

    if (std::abs(clampedZoom - zoom_) < 0.001f) {
        return true;
    }

    std::optional<POINT> anchor;
    RECT clientRect {};
    GetClientRect(window_, &clientRect);

    POINT cursor {};
    if (GetCursorPos(&cursor) != FALSE) {
        ScreenToClient(window_, &cursor);
        if (PtInRect(&clientRect, cursor) != 0) {
            anchor = cursor;
        }
    }

    if (!anchor.has_value()) {
        anchor = POINT {
            (clientRect.left + clientRect.right) / 2,
            (clientRect.top + clientRect.bottom) / 2,
        };
    }

    if (toolbarZoomEditing_) {
        CancelToolbarZoomInput();
    }
    CancelDeferredRasterRefresh();
    BeginZoomSession(*anchor);
    zoom_ = clampedZoom;
    targetZoom_ = clampedZoom;

    if (zoomAnchorValid_) {
        const POINT anchoredScreen = PageToScreen(zoomAnchorPageIndex_, zoomAnchorPage_);
        panX_ += static_cast<float>(zoomAnchorCursor_.x - anchoredScreen.x);
        panY_ += static_cast<float>(zoomAnchorCursor_.y - anchoredScreen.y);
    }

    InvalidateViewport();
    RECT toolbarRect = clientRect;
    toolbarRect.bottom = ToolbarHeight(IsZoomed(window_), WindowDpi(window_));
    InvalidateRect(window_, &toolbarRect, FALSE);
    return true;
}

void App::BeginZoomSession(POINT cursor) {
    const bool startingNewSession = !zoomSessionActive_;
    zoomSessionActive_ = true;
    zoomAnchorValid_ = false;

    if (startingNewSession) {
        ++renderGeneration_;
        pendingRenderScales_.clear();
        {
            std::lock_guard lock(renderMutex_);
            renderRequests_.clear();
            renderResults_.clear();
        }
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    if (const auto hitPage = ResolveZoomAnchorPage(clientRect, cursor); hitPage.has_value()) {
        zoomAnchorValid_ = true;
        zoomAnchorPageIndex_ = hitPage->pageIndex;
        zoomAnchorPage_ = ScreenToPage(*hitPage, cursor, false);
        zoomAnchorCursor_ = cursor;
    }

    if (startingNewSession) {
        CaptureZoomSessionSnapshots();
    }

    SetTimer(window_, kZoomSessionTimerId, kZoomSessionDelayMs, nullptr);
}

void App::ZoomAt(int delta, POINT cursor) {
    const float factor = delta > 0 ? 1.1f : 0.9f;
    const float newZoom = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);

    if (std::abs(newZoom - zoom_) < 0.0001f) {
        return;
    }

    if (toolbarZoomEditing_) {
        CancelToolbarZoomInput();
    }
    CancelDeferredRasterRefresh();
    BeginZoomSession(cursor);
    zoom_ = newZoom;
    targetZoom_ = newZoom;

    if (zoomAnchorValid_) {
        const POINT anchoredScreen = PageToScreen(zoomAnchorPageIndex_, zoomAnchorPage_);
        panX_ += static_cast<float>(zoomAnchorCursor_.x - anchoredScreen.x);
        panY_ += static_cast<float>(zoomAnchorCursor_.y - anchoredScreen.y);
    }

    InvalidateViewport();
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    RECT toolbarRect = clientRect;
    toolbarRect.bottom = ToolbarHeight(IsZoomed(window_), WindowDpi(window_));
    InvalidateRect(window_, &toolbarRect, FALSE);
}

void App::EndZoomSession() {
    if (!zoomSessionActive_) {
        return;
    }

    zoomSessionActive_ = false;
    zoomAnchorValid_ = false;
    KillTimer(window_, kZoomSessionTimerId);
    ClearZoomSessionSnapshots();
    InvalidateCanvas();
    ScheduleDeferredRasterRefresh();
}

}  // namespace cnote
