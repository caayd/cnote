#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>

#include <QSize>

namespace cnote::render {

// Caps for one raster allocation.  A render that would exceed either limit is
// refused before QImage is asked to allocate it, so a zoom/DPR combination
// cannot request a multi-gigabyte surface (the old code built the full raster,
// which either failed every paint or exhausted memory).
inline constexpr int kMaxRasterDimension = 8192;
inline constexpr std::size_t kMaxPageSurfaceBytes = 24U * 1024U * 1024U;

// Aggregate byte budget for the canvas' cached page rasters (full surfaces and
// tiles).  The canvas keeps nearby pages until this is reached instead of
// evicting every page that leaves the viewport.
inline constexpr std::size_t kCanvasSurfaceCacheBytes = 96U * 1024U * 1024U;

// Raster bytes one paint may spend on new page surfaces before deferring the
// rest to the next frame (zoom settle replaces visible pages across frames).
inline constexpr std::size_t kSurfaceRenderBytesPerPaint = 24U * 1024U * 1024U;

// Edge length of one page tile in device pixels.  Pages whose full-page raster
// exceeds the budget render visible tiles of this size instead.
inline constexpr int kPageTileSize = 1024;

// Aggregate byte budget for the lazily rendered PDF page backgrounds.
inline constexpr std::size_t kPdfBackgroundCacheBytes = 48U * 1024U * 1024U;

struct RasterBudget {
    int maxDimension = kMaxRasterDimension;
    std::size_t maxSurfaceBytes = kMaxPageSurfaceBytes;
};

// Bytes an ARGB32 raster of these dimensions needs, saturating instead of
// overflowing for absurd inputs.
[[nodiscard]] constexpr std::size_t RasterBytes(long long width, long long height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    const long long pixels = width * height;
    if (pixels > static_cast<long long>(std::numeric_limits<std::size_t>::max() / 4U)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(pixels) * 4U;
}

[[nodiscard]] constexpr bool RasterFitsBudget(
    long long width, long long height, RasterBudget budget = RasterBudget {}) {
    return width > 0 && height > 0
        && width <= budget.maxDimension && height <= budget.maxDimension
        && RasterBytes(width, height) <= budget.maxSurfaceBytes;
}

// Rounds a page/logical size in raster pixels, or nullopt when the result is
// not a usable allocation candidate: non-finite, non-positive, past the
// dimension cap, or past the int range (the old path cast a huge double to int,
// which is undefined).
[[nodiscard]] inline std::optional<QSize> CheckedRasterSize(
    double width, double height, RasterBudget budget = RasterBudget {}) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0) {
        return std::nullopt;
    }
    if (width > static_cast<double>(budget.maxDimension)
        || height > static_cast<double>(budget.maxDimension)) {
        return std::nullopt;
    }
    const int roundedWidth = std::max(1, static_cast<int>(std::lround(width)));
    const int roundedHeight = std::max(1, static_cast<int>(std::lround(height)));
    if (!RasterFitsBudget(roundedWidth, roundedHeight, budget)) {
        return std::nullopt;
    }
    return QSize(roundedWidth, roundedHeight);
}

}  // namespace cnote::render
