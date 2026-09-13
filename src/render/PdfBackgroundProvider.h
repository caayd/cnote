#pragma once

#include <cstdint>

#include <QImage>

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
};

}  // namespace cnote::render
