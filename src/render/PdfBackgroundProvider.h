#pragma once

#include <cstdint>
#include <optional>

#include <QImage>
#include <QRect>

namespace cnote::render {

// Supplies PDF page backgrounds lazily for renderers that paint Page::raster.
// Implementations render on demand and cache by (source, page, raster scale),
// so a multi-page PDF never materializes every page up front.
class PdfBackgroundProvider {
public:
    virtual ~PdfBackgroundProvider() = default;

    // Returns the background for a PDF source page at the requested raster
    // scale (page units -> raster pixels), or a null image when unavailable.
    [[nodiscard]] virtual QImage PageBackground(std::int32_t sourceIndex, std::int32_t pageIndex, float rasterScale) = 0;

    // Returns only `pixelRect` of that same background raster (top-down device
    // pixels of the full-page background), or null when the provider has no
    // bounded-region path.  Region renders keep a page surface or tile inside
    // the raster budget instead of materializing the whole background.
    [[nodiscard]] virtual std::optional<QImage> PageBackgroundRegion(
        std::int32_t sourceIndex, std::int32_t pageIndex, float rasterScale, const QRect& pixelRect) {
        static_cast<void>(sourceIndex);
        static_cast<void>(pageIndex);
        static_cast<void>(rasterScale);
        static_cast<void>(pixelRect);
        return std::nullopt;
    }
};

}  // namespace cnote::render
