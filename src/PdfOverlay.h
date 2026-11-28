#pragma once

#include <cstddef>
#include <vector>

#include "Document.h"

namespace cnote {

class PdfiumBackend;

// Backend-specific page raster composition for PDF export. Defined once in
// src/render/PdfExportQt.cpp; the export orchestration in PdfBackend.cpp is
// shared and only the page raster composition lives here.

// Shared raster allocation helper (defined in PdfBackend.cpp).
[[nodiscard]] RasterImage CreateBlankRaster(float pageWidth, float pageHeight, float renderScale);

// True when every pixel's alpha byte is 0xFF, so the raster can be handed to
// PDFium as an opaque bitmap without an alpha mask.
[[nodiscard]] bool IsOpaqueBgra(const RasterImage& raster);

// A raster placed on the page. The rectangle is in page units with the page's
// top-left origin, exactly the space PageText/PageLatex use; the caller maps it
// to the PDF image matrix (whose y axis points up).
struct PlacedRaster {
    RasterImage image {};
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    // Every pixel is opaque, so the PDF writer may use an opaque bitmap.
    bool opaque = false;
};

// Renders each text/LaTeX object of a page into its own transparent raster,
// cropped to that object's painted bounds. Returned in paint order (all texts,
// then all LaTeX), so appending them in order preserves the old overlay's
// layer order without allocating a page-sized image for a small object.
[[nodiscard]] std::vector<PlacedRaster> ComposeTextOverlayRasters(const Page& page, float renderScale);

// Flattens a page (PDF source raster or stored raster, images, optional
// strokes) into an opaque raster covering the whole page.
[[nodiscard]] PlacedRaster ComposePageRaster(
    const PdfiumBackend& backend,
    const Document& document,
    std::size_t pageIndex,
    float renderScale,
    float strokeSmoothing,
    float dashLength,
    float dashGap,
    bool includeStrokes);

}  // namespace cnote
