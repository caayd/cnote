#include "App.h"

#include <algorithm>
#include <cmath>

#include <dcomp.h>

#include "DpiUtil.h"
#include "ui/UiUtil.h"

namespace cnote {

namespace {

using ui::MakeRect;

int ViewportFrameThicknessX(UINT dpi) {
    return ScaleForDpi(8, dpi);
}

float DistanceSquaredToRect(const RECT& rect, POINT point) {
    const float clampedX = static_cast<float>(std::clamp(point.x, rect.left, rect.right - 1));
    const float clampedY = static_cast<float>(std::clamp(point.y, rect.top, rect.bottom - 1));
    const float dx = static_cast<float>(point.x) - clampedX;
    const float dy = static_cast<float>(point.y) - clampedY;
    return (dx * dx) + (dy * dy);
}

int MaximizedCanvasWidthForZoom(HWND window, int fallbackCanvasWidth, int sidebarWidth, UINT dpi) {
    if (window == nullptr) {
        return fallbackCanvasWidth;
    }

    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || GetMonitorInfoW(monitor, &monitorInfo) == FALSE) {
        return fallbackCanvasWidth;
    }

    const int workAreaWidth = std::max(1, static_cast<int>(monitorInfo.rcWork.right - monitorInfo.rcWork.left));
    const int maximizedHorizontalInset = ViewportFrameThicknessX(dpi) * 2;
    return std::max(1, workAreaWidth - maximizedHorizontalInset - std::max(0, sidebarWidth));
}

float SmoothScrollInterpolation(float start, float target, double progress) {
    const double clampedProgress = std::clamp(progress, 0.0, 1.0);
    const double eased = (3.0 * clampedProgress * clampedProgress) - (2.0 * clampedProgress * clampedProgress * clampedProgress);
    return start + static_cast<float>((target - start) * eased);
}

}  // namespace

void App::ResetView() {
    CancelToolbarZoomInput();
    StopSmoothScroll();
    zoom_ = 1.0f;
    targetZoom_ = 1.0f;
    panX_ = 0.0f;
    panY_ = 0.0f;
    if (window_ != nullptr) {
        RECT clientRect {};
        GetClientRect(window_, &clientRect);
        RECT toolbarRect = clientRect;
        toolbarRect.bottom = FullToolbarHeight();
        InvalidateRect(window_, &toolbarRect, FALSE);
    }
}

void App::FocusPage(std::size_t index) {
    if (index >= document_.PageCount() || window_ == nullptr) {
        return;
    }

    document_.SetCurrentPageIndex(index);
    MarkPdfSidebarCompositionDirty();

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, index);
    if (!pageView.valid) {
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    EnsureSidebarCurrentPageVisible();
    StopSmoothScroll();
    const int toolbarHeight = ReservedToolbarHeight();
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
    const int horizontalInset = IsZoomed(window_) ? ViewportFrameThicknessX(dpi) : 0;
    rect.top = ReservedToolbarHeight();
    rect.left += horizontalInset;
    rect.right -= horizontalInset;
    return rect;
}

void App::PanBy(float dx, float dy) {
    StopSmoothScroll();
    ApplyPanDelta(dx, dy);
}

void App::ApplyPanDelta(float dx, float dy) {
    panX_ += dx;
    panY_ += dy;
    InvalidateViewport();
}

void App::SmoothPanBy(float dx, float dy) {
    if (window_ == nullptr) {
        ApplyPanDelta(dx, dy);
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);

    float startOffsetX = 0.0f;
    float startOffsetY = 0.0f;
    if (smoothScrollActive_) {
        startOffsetX = CurrentSmoothScrollOffsetX();
        startOffsetY = CurrentSmoothScrollOffsetY();
        const float nextOffsetX = startOffsetX + dx;
        const float nextOffsetY = startOffsetY + dy;
        const float maxOffsetX = static_cast<float>(std::max(1, contentCompositionPaddingX_ - 48));
        const float maxOffsetY = static_cast<float>(std::max(1, contentCompositionPaddingY_ - 48));
        if (std::abs(nextOffsetX) > maxOffsetX || std::abs(nextOffsetY) > maxOffsetY) {
            StopSmoothScroll();
            SmoothPanBy(dx, dy);
            return;
        }
    } else {
        smoothScrollActive_ = true;
        contentCompositionDirty_ = true;
        if (!UpdateContentComposition(clientRect, clientRect)) {
            smoothScrollActive_ = false;
            ApplyPanDelta(dx, dy);
            return;
        }
    }

    const float targetOffsetX = startOffsetX + dx;
    const float targetOffsetY = startOffsetY + dy;
    if (!StartSmoothScrollCompositionAnimation(startOffsetX, startOffsetY, targetOffsetX, targetOffsetY)) {
        StopSmoothScroll();
        ApplyPanDelta(dx, dy);
        return;
    }

    smoothScrollStartOffsetX_ = startOffsetX;
    smoothScrollStartOffsetY_ = startOffsetY;
    smoothScrollTargetOffsetX_ = targetOffsetX;
    smoothScrollTargetOffsetY_ = targetOffsetY;
    smoothScrollAnimationStartMs_ = GetTickCount64();
    StartSmoothScrollTimer();
}

void App::StartSmoothScrollTimer() {
    if (window_ == nullptr) {
        return;
    }

    KillTimer(window_, kSmoothScrollTimerId);
    SetTimer(window_, kSmoothScrollTimerId, kSmoothScrollAnimationDurationMs + 20, nullptr);
}

void App::StopSmoothScroll() {
    if (window_ != nullptr) {
        KillTimer(window_, kSmoothScrollTimerId);
    }

    const float offsetX = CurrentSmoothScrollOffsetX();
    const float offsetY = CurrentSmoothScrollOffsetY();
    const bool hadVisualOffset = smoothScrollActive_ && (std::abs(offsetX) > 0.001f || std::abs(offsetY) > 0.001f);
    smoothScrollActive_ = false;
    smoothScrollAnimationStartMs_ = 0;
    smoothScrollStartOffsetX_ = 0.0f;
    smoothScrollStartOffsetY_ = 0.0f;
    smoothScrollTargetOffsetX_ = 0.0f;
    smoothScrollTargetOffsetY_ = 0.0f;

    if (!hadVisualOffset) {
        return;
    }

    panX_ += offsetX;
    panY_ += offsetY;
    contentCompositionDirty_ = true;

    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    if (!UpdateContentComposition(clientRect, clientRect)) {
        InvalidateViewport();
    }
}

void App::HandleSmoothScrollTimer() {
    if (window_ == nullptr || !smoothScrollActive_ || IsDocumentInteractionActive() || isPanning_) {
        StopSmoothScroll();
        return;
    }

    StopSmoothScroll();
}

float App::CurrentSmoothScrollOffsetX() const {
    if (!smoothScrollActive_) {
        return 0.0f;
    }
    if (smoothScrollAnimationStartMs_ == 0) {
        return smoothScrollTargetOffsetX_;
    }

    const ULONGLONG now = GetTickCount64();
    const double durationMs = static_cast<double>(kSmoothScrollAnimationDurationMs);
    const double elapsedMs = std::clamp(static_cast<double>(now - smoothScrollAnimationStartMs_), 0.0, durationMs);
    return SmoothScrollInterpolation(smoothScrollStartOffsetX_, smoothScrollTargetOffsetX_, elapsedMs / durationMs);
}

float App::CurrentSmoothScrollOffsetY() const {
    if (!smoothScrollActive_) {
        return 0.0f;
    }
    if (smoothScrollAnimationStartMs_ == 0) {
        return smoothScrollTargetOffsetY_;
    }

    const ULONGLONG now = GetTickCount64();
    const double durationMs = static_cast<double>(kSmoothScrollAnimationDurationMs);
    const double elapsedMs = std::clamp(static_cast<double>(now - smoothScrollAnimationStartMs_), 0.0, durationMs);
    return SmoothScrollInterpolation(smoothScrollStartOffsetY_, smoothScrollTargetOffsetY_, elapsedMs / durationMs);
}

bool App::StartSmoothScrollCompositionAnimation(float startOffsetX, float startOffsetY, float targetOffsetX, float targetOffsetY) {
    if (window_ == nullptr) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    if (!UpdateContentComposition(clientRect, clientRect) || compositionDevice_ == nullptr || contentCompositionVisual_ == nullptr) {
        return false;
    }

    const auto animateAxis = [&](float startOffset, float targetOffset, bool horizontal) {
        const float baseOffset = horizontal ? static_cast<float>(-contentCompositionPaddingX_) : static_cast<float>(-contentCompositionPaddingY_);
        const float startValue = baseOffset + startOffset;
        const float targetValue = baseOffset + targetOffset;
        if (std::abs(startValue - targetValue) <= 0.001f) {
            if (horizontal) {
                contentCompositionVisual_->SetOffsetX(targetValue);
            } else {
                contentCompositionVisual_->SetOffsetY(targetValue);
            }
            return S_OK;
        }

        IDCompositionAnimation* animation = nullptr;
        HRESULT hr = compositionDevice_->CreateAnimation(&animation);
        if (FAILED(hr) || animation == nullptr) {
            ReleaseCom(animation);
            return hr;
        }

        const double durationSeconds = static_cast<double>(kSmoothScrollAnimationDurationMs) / 1000.0;
        const float delta = targetValue - startValue;
        hr = animation->Reset();
        if (SUCCEEDED(hr)) {
            hr = animation->AddCubic(
                0.0,
                startValue,
                0.0f,
                delta * static_cast<float>(3.0 / (durationSeconds * durationSeconds)),
                delta * static_cast<float>(-2.0 / (durationSeconds * durationSeconds * durationSeconds)));
        }
        if (SUCCEEDED(hr)) {
            hr = animation->End(durationSeconds, targetValue);
        }
        if (SUCCEEDED(hr)) {
            hr = horizontal ? contentCompositionVisual_->SetOffsetX(animation) : contentCompositionVisual_->SetOffsetY(animation);
        }
        ReleaseCom(animation);
        return hr;
    };

    if (FAILED(animateAxis(startOffsetX, targetOffsetX, true)) || FAILED(animateAxis(startOffsetY, targetOffsetY, false))) {
        StopSmoothScroll();
        return false;
    }

    compositionDevice_->Commit();
    return true;
}

void App::SetZoom(float zoom, std::optional<POINT> anchor) {
    StopSmoothScroll();
    if (window_ == nullptr) {
        zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
        targetZoom_ = zoom_;
        return;
    }

    const float clampedZoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    if (std::abs(clampedZoom - zoom_) < 0.001f) {
        InvalidateRect(window_, nullptr, FALSE);
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

    BeginZoomSession(focusPoint);
    targetZoom_ = clampedZoom;
    zoom_ = clampedZoom;

    if (zoomAnchorValid_) {
        const POINT anchoredScreen = PageToScreen(zoomAnchorPageIndex_, zoomAnchorPage_);
        panX_ += static_cast<float>(zoomAnchorCursor_.x - anchoredScreen.x);
        panY_ += static_cast<float>(zoomAnchorCursor_.y - anchoredScreen.y);
    }

    InvalidateViewport();
    RECT toolbarRect = clientRect;
    toolbarRect.bottom = FullToolbarHeight();
    InvalidateRect(window_, &toolbarRect, FALSE);
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
    const int zoomReferenceCanvasWidth = MaximizedCanvasWidthForZoom(window_, canvasWidth, 0, dpi);
    float widestPage = 1.0f;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        widestPage = std::max(widestPage, document_.PageAt(pageIndex).width);
    }

    const float fitScale = static_cast<float>(zoomReferenceCanvasWidth - ScaleForDpi(120, dpi)) / widestPage;
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

bool App::ApplyZoomPreset(std::size_t presetIndex, std::optional<POINT> preferredAnchor) {
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

    if (preferredAnchor.has_value() && PtInRect(&clientRect, *preferredAnchor) != 0) {
        anchor = *preferredAnchor;
    }

    if (!anchor.has_value()) {
        POINT cursor {};
        if (GetCursorPos(&cursor) != FALSE) {
            ScreenToClient(window_, &cursor);
            if (PtInRect(&clientRect, cursor) != 0) {
                anchor = cursor;
            }
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
    StopSmoothScroll();
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
    toolbarRect.bottom = FullToolbarHeight();
    InvalidateRect(window_, &toolbarRect, FALSE);
    return true;
}

void App::BeginZoomSession(POINT cursor) {
    const bool startingNewSession = !zoomSessionActive_;
    zoomSessionActive_ = true;
    zoomAnchorValid_ = false;

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
    StopSmoothScroll();
    const float factor = delta > 0 ? 1.1f : 0.9f;
    const float newZoom = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);

    if (std::abs(newZoom - zoom_) < 0.0001f) {
        return;
    }

    if (toolbarZoomEditing_) {
        CancelToolbarZoomInput();
    }
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
    toolbarRect.bottom = FullToolbarHeight();
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
}

}  // namespace cnote
