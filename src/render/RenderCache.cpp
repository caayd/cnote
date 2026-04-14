#include "App.h"

#include <algorithm>
#include <array>

#include "DpiUtil.h"

namespace cnote {

namespace {

constexpr int kVisibleRasterMargin = 240;
constexpr int kRasterUnloadMargin = 32000;
constexpr float kRasterUpgradeThreshold = 1.25f;
constexpr float kRasterDowngradeThreshold = 1.35f;

float QuantizeRenderScale(float renderScale) {
    constexpr std::array<float, 7> kRenderScaleBuckets { 1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f };
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

        if (result.generation != renderGeneration_ || result.pageIndex >= document_.PageCount()) {
            continue;
        }

        const auto pending = pendingRenderScales_.find(result.pageIndex);
        if (pending == pendingRenderScales_.end()) {
            continue;
        }
        if (pending->second > result.renderScale + 0.001f || pending->second < result.renderScale - 0.001f) {
            continue;
        }
        if (!result.raster.has_value()) {
            pendingRenderScales_.erase(pending);
            continue;
        }

        pendingRenderScales_.erase(pending);
        Page& page = document_.PageAt(result.pageIndex);
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
    const bool needsRefresh = missingRaster
        || currentScale <= 0.0f
        || desiredScale > (currentScale * kRasterUpgradeThreshold)
        || currentScale > (desiredScale * kRasterDowngradeThreshold);
    if (!needsRefresh) {
        return;
    }

    const auto pending = pendingRenderScales_.find(pageView.pageIndex);
    if (pending != pendingRenderScales_.end()
        && pending->second <= desiredScale + 0.001f
        && pending->second >= desiredScale - 0.001f) {
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
                        && request.pageIndex == pageView.pageIndex;
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
    const int visibleTop = ReservedToolbarHeight() - ScaleForDpi(kVisibleRasterMargin, dpi);
    const int visibleBottom = static_cast<int>(clientRect.bottom) + ScaleForDpi(kVisibleRasterMargin, dpi);
    std::vector<bool> keepRaster(document_.PageCount(), false);
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }
        if (pageView.rect.bottom < visibleTop || pageView.rect.top > visibleBottom) {
            continue;
        }
        keepRaster[pageView.pageIndex] = true;
        QueuePageRasterRequest(pageView);
    }

    const int keepTop = ReservedToolbarHeight() - ScaleForDpi(kRasterUnloadMargin, dpi);
    const int keepBottom = static_cast<int>(clientRect.bottom) + ScaleForDpi(kRasterUnloadMargin, dpi);
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }
        if (pageView.rect.bottom >= keepTop && pageView.rect.top <= keepBottom) {
            keepRaster[pageView.pageIndex] = true;
        }
    }

    for (std::size_t pageIndex = 0; pageIndex < keepRaster.size(); ++pageIndex) {
        if (!keepRaster[pageIndex]) {
            pendingRenderScales_.erase(pageIndex);
            {
                std::lock_guard lock(renderMutex_);
                renderRequests_.erase(
                    std::remove_if(
                        renderRequests_.begin(),
                        renderRequests_.end(),
                        [&](const RenderRequest& request) {
                            return request.generation == renderGeneration_
                                && request.pageIndex == pageIndex;
                        }),
                    renderRequests_.end());
                renderResults_.erase(
                    std::remove_if(
                        renderResults_.begin(),
                        renderResults_.end(),
                        [&](const RenderResult& result) {
                            return result.generation == renderGeneration_
                                && result.pageIndex == pageIndex;
                        }),
                    renderResults_.end());
            }
            document_.PageAt(pageIndex).raster = {};
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

    constexpr float kRenderOverscan = 1.05f;
    constexpr float kMinRenderScale = 1.0f;
    constexpr float kMaxRenderScale = 4.0f;
    const UINT dpi = window_ != nullptr ? WindowDpi(window_) : SystemDpi();
    const float logicalPageScale = pageView.scale * (static_cast<float>(USER_DEFAULT_SCREEN_DPI) / static_cast<float>(std::max(dpi, static_cast<UINT>(USER_DEFAULT_SCREEN_DPI))));
    const float clampedScale = std::clamp(logicalPageScale * kRenderOverscan, kMinRenderScale, kMaxRenderScale);
    return QuantizeRenderScale(clampedScale);
}

}  // namespace cnote
