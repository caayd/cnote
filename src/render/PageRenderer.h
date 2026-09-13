#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <QImage>
#include <QPointF>

#include "Document.h"

class QPainter;

namespace cnote::render {

// Committed-object render parameters. `scale` maps page units to painter units;
// `devicePixelRatio` is applied only when allocating a raster surface.
struct PageRenderParams {
    float scale = 1.0f;
    float devicePixelRatio = 1.0f;
    float strokeSmoothing = 0.55f;
    float dashLength = 3.0f;
    float dashGap = 2.0f;
    // Committed objects excluded from this render (e.g. the object being
    // edited). Excluded objects are also excluded from cache signatures.
    std::optional<std::size_t> excludedTextIndex;
    std::optional<std::size_t> excludedLatexIndex;
    std::optional<std::size_t> excludedStrokeIndex;
};

// Draws the committed objects of one page in layer order: page/PDF background,
// images, committed text, LaTeX, strokes. `pageOrigin` is the page's top-left
// in the painter's coordinate space; the painter's state is preserved.
void PaintPage(QPainter& painter, const Page& page, const QPointF& pageOrigin, const PageRenderParams& params);

// Draws one stroke in page units for a transient preview (live stroke, pending
// erase). Same pen/path/dash/opacity rules as committed strokes; `params.scale`
// maps page units to painter units exactly like PaintPage.
void PaintStrokePreview(QPainter& painter, const Stroke& stroke, const PageRenderParams& params, float opacity);

// Renders a page into a top-down premultiplied ARGB32 QImage sized
// page.width * scale * devicePixelRatio.
[[nodiscard]] QImage RenderPageSurface(const Page& page, const PageRenderParams& params);

// Renders a page thumbnail into a QImage of the given width (height from the
// page aspect ratio).
[[nodiscard]] QImage RenderPageThumbnail(const Page& page, int width);

// Cached page surface owned by the caller. EnsurePageSurface re-renders only
// when the signature changes.
struct PageSurfaceCache {
    QImage image;
    std::uint64_t signature = 0;
    bool valid = false;
};

[[nodiscard]] std::uint64_t PageSurfaceSignature(const Page& page, const PageRenderParams& params);
void EnsurePageSurface(PageSurfaceCache& cache, const Page& page, const PageRenderParams& params);

// Wraps top-down straight-alpha BGRA bytes (model/PDFium layout) as a
// non-owning ARGB32 QImage view. The caller must keep `pixels` alive for as
// long as the returned image is used.
[[nodiscard]] QImage WrapBgraImage(const std::uint8_t* pixels, int width, int height, int stride);

}  // namespace cnote::render
