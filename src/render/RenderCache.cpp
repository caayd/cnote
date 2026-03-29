#include "App.h"

#include <algorithm>
#include <array>

#include "DpiUtil.h"

namespace cnote {

namespace {

constexpr int kBaseToolbarHeight = 34;
constexpr int kMaximizedToolbarExtraHeight = 5;

int ToolbarHeight(bool maximized, UINT dpi) {
    return ScaleForDpi(kBaseToolbarHeight + (maximized ? kMaximizedToolbarExtraHeight : 0), dpi);
}

float QuantizeRenderScale(float renderScale) {
    constexpr std::array<float, 5> kRenderScaleBuckets { 1.5f, 2.0f, 3.0f, 4.0f, 6.0f };
    for (const float bucket : kRenderScaleBuckets) {
        if (renderScale <= bucket + 0.001f) {
            return bucket;
        }
    }
    return kRenderScaleBuckets.back();
}

}  // namespace

void App::ScheduleDeferredRasterRefresh() {
    if (window_ == nullptr || zoomSessionActive_) {
        return;
    }

    rasterRefreshPending_ = true;
    SetTimer(window_, kRasterRefreshTimerId, kRasterRefreshDelayMs, nullptr);
}

void App::CancelDeferredRasterRefresh(bool clearPending) {
    if (window_ == nullptr) {
        return;
    }

    KillTimer(window_, kRasterRefreshTimerId);
    if (clearPending) {
        rasterRefreshPending_ = false;
    }
}

void App::HandleDeferredRasterRefresh() {
    if (isDrawing_ || isPanning_ || isErasing_ || isMovingSelection_ || isTransformingImage_ || isTransformingStroke_ || zoomSessionActive_) {
        SetTimer(window_, kRasterRefreshTimerId, kRasterRefreshDelayMs, nullptr);
        return;
    }

    CancelDeferredRasterRefresh();
    InvalidateVisiblePageSurfaceCaches();
    EnsureVisiblePageRasterQuality();
    InvalidateCanvas();
}

void App::HandleCompletedRenderResults() {
    std::deque<RenderResult> results;
    {
        std::lock_guard lock(renderMutex_);
        results.swap(renderResults_);
    }

    bool updated = false;
    while (!results.empty()) {
        RenderResult result = std::move(results.front());
        results.pop_front();

        if (result.pageIndex < document_.PageCount()) {
            const auto pending = pendingRenderScales_.find(result.pageIndex);
            if (pending != pendingRenderScales_.end() && pending->second <= result.renderScale + 0.001f) {
                pendingRenderScales_.erase(pending);
            }
        }

        const auto pending = pendingRenderScales_.find(result.pageIndex);
        if (pending != pendingRenderScales_.end() && pending->second > result.renderScale + 0.001f) {
            continue;
        }

        if (result.generation != renderGeneration_ || !result.raster.has_value() || result.pageIndex >= document_.PageCount()) {
            continue;
        }

        Page& page = document_.PageAt(result.pageIndex);
        if (result.renderScale + 0.001f < page.raster.scale) {
            continue;
        }

        page.raster = std::move(*result.raster);
        if (!zoomSessionActive_) {
            InvalidatePageSurfaceCache(result.pageIndex);
        } else {
            InvalidateSidebarThumbnailCache(result.pageIndex);
        }
        updated = true;
    }

    if (updated && !zoomSessionActive_) {
        InvalidateCanvas();
    }
}

void App::QueuePageRasterRequest(const PageView& pageView) {
    if (!pageView.valid) {
        return;
    }

    Page& page = document_.PageAt(pageView.pageIndex);
    const Document::PdfSource* pdfSource = document_.PdfSourceAtIndex(page.sourcePdfSourceIndex);
    if (pdfSource == nullptr || page.sourcePdfPageIndex < 0) {
        return;
    }
    const float desiredScale = DesiredRenderScale(pageView);
    const float currentScale = page.raster.scale;
    const bool missingRaster = page.raster.pixels.empty() || page.raster.width <= 0 || page.raster.height <= 0;
    const bool needsUpgrade = missingRaster || currentScale <= 0.0f || desiredScale > (currentScale * 1.2f);
    if (!needsUpgrade) {
        return;
    }

    const auto pending = pendingRenderScales_.find(pageView.pageIndex);
    if (pending != pendingRenderScales_.end() && pending->second >= desiredScale - 0.001f) {
        return;
    }

    pendingRenderScales_[pageView.pageIndex] = desiredScale;
    {
        std::lock_guard lock(renderMutex_);
        renderRequests_.erase(
            std::remove_if(
                renderRequests_.begin(),
                renderRequests_.end(),
                [&](const RenderRequest& request) {
                    return request.generation == renderGeneration_
                        && request.pageIndex == pageView.pageIndex
                        && request.renderScale <= desiredScale + 0.001f;
                }),
            renderRequests_.end());
        renderRequests_.push_back({ pdfSource->pdfData, pageView.pageIndex, page.sourcePdfPageIndex, desiredScale, renderGeneration_ });
    }
    renderCv_.notify_one();
}

void App::EnsureVisiblePageRasterQuality() {
    if (!document_.IsPdfDocument() || !document_.HasPdfData()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const UINT dpi = WindowDpi(window_);
    const int visibleTop = ToolbarHeight(IsZoomed(window_), dpi) - ScaleForDpi(200, dpi);
    const int visibleBottom = static_cast<int>(clientRect.bottom) + ScaleForDpi(200, dpi);
    std::vector<bool> keepCache(document_.PageCount(), false);
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }
        if (pageView.rect.bottom < visibleTop || pageView.rect.top > visibleBottom) {
            continue;
        }
        keepCache[pageView.pageIndex] = true;
        QueuePageRasterRequest(pageView);
    }

    const int cacheKeepTop = visibleTop - ScaleForDpi(1200, dpi);
    const int cacheKeepBottom = visibleBottom + ScaleForDpi(1200, dpi);
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }
        if (pageView.rect.bottom >= cacheKeepTop && pageView.rect.top <= cacheKeepBottom) {
            keepCache[pageView.pageIndex] = true;
        }
    }

    for (std::size_t pageIndex = 0; pageIndex < keepCache.size(); ++pageIndex) {
        if (!keepCache[pageIndex]) {
            InvalidatePageSurfaceCache(pageIndex);
        }
    }
}

void App::EnsurePageRasterQuality(const PageView& pageView) {
    QueuePageRasterRequest(pageView);
}

float App::DesiredRenderScale(const PageView& pageView) const {
    if (!pageView.valid) {
        return 0.0f;
    }

    constexpr float kRenderOverscan = 1.2f;
    constexpr float kMinRenderScale = 1.5f;
    constexpr float kMaxRenderScale = 6.0f;
    const float clampedScale = std::clamp(pageView.scale * kRenderOverscan, kMinRenderScale, kMaxRenderScale);
    return QuantizeRenderScale(clampedScale);
}

}  // namespace cnote
