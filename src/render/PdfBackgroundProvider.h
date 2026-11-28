#pragma once

#include <cstdint>
#include <optional>

#include <QImage>
#include <QRect>
#include <QSize>

namespace cnote::render {

// Which consumer a background is for. Thumbnails render at a much smaller
// scale and churn through many pages while scrolling the sidebar; keeping
// their cached rasters in a separate, smaller budget means that churn cannot
// evict the full-scale canvas surfaces the user is looking at.
enum class PdfBackgroundUse : std::uint8_t {
    Canvas,
    Thumbnail,
};

// Supplies PDF page backgrounds lazily for renderers that paint Page::raster.
// Implementations render on demand and cache by (source, page, raster scale),
// so a multi-page PDF never materializes every page up front.
class PdfBackgroundProvider {
public:
    virtual ~PdfBackgroundProvider() = default;

    // Returns the background for a PDF source page at the requested raster
    // scale (page units -> raster pixels), or a null image when unavailable.
    [[nodiscard]] virtual QImage PageBackground(std::int32_t sourceIndex, std::int32_t pageIndex,
        float rasterScale, PdfBackgroundUse use = PdfBackgroundUse::Canvas) = 0;

    // Returns only `pixelRect` of that same background raster (top-down device
    // pixels of the full-page background), or null when the provider has no
    // bounded-region path.  Region renders keep a page surface or tile inside
    // the raster budget instead of materializing the whole background.
    //
    // `fullPagePixelSize` is the size that full-page background raster would
    // have (the space `pixelRect` is measured in).  A miss crops a warm
    // full-page raster when one exists; with that size the provider may also
    // render the full page once into its cache when it fits the raster budget,
    // so later patches are exact crops of what the page surface blitted.
    // Without it (or above the budget) a miss renders just the requested
    // region.  Implementations without a region path keep the base fallback.
    [[nodiscard]] virtual std::optional<QImage> PageBackgroundRegion(
        std::int32_t sourceIndex, std::int32_t pageIndex, float rasterScale, const QRect& pixelRect,
        QSize fullPagePixelSize = {}) {
        static_cast<void>(sourceIndex);
        static_cast<void>(pageIndex);
        static_cast<void>(rasterScale);
        static_cast<void>(pixelRect);
        static_cast<void>(fullPagePixelSize);
        return std::nullopt;
    }
};

}  // namespace cnote::render
