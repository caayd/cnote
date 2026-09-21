#pragma once

#include <cstddef>
#include <optional>

#include "Document.h"

namespace cnote {

class PdfiumBackend;

// Backend-specific page raster composition for PDF export. Defined once in
// src/render/PdfExportQt.cpp; the export orchestration in PdfBackend.cpp is
// shared and only the page raster composition lives here.

// Shared raster allocation helper (defined in PdfBackend.cpp).
[[nodiscard]] RasterImage CreateBlankRaster(float pageWidth, float pageHeight, float renderScale);

// Renders the text/LaTeX overlay of a page into a transparent raster.
[[nodiscard]] std::optional<RasterImage> ComposeTextOverlayRaster(const Page& page, float renderScale);

// Flattens a page (PDF source raster or stored raster, images, optional
// strokes) into a raster for pages that have no imported vector source.
[[nodiscard]] RasterImage ComposePageRaster(
    const PdfiumBackend& backend,
    const Document& document,
    std::size_t pageIndex,
    float renderScale,
    float strokeSmoothing,
    float dashLength,
    float dashGap,
    bool includeStrokes);

}  // namespace cnote
