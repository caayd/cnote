#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <QImage>
#include <QPointF>
#include <QRectF>

#include "Document.h"

class QPainter;

namespace cnote::render {

class PdfBackgroundProvider;

// Opacity committed objects render at while the eraser drag has marked them
// for deletion (App.cpp:172 kPendingEraseObjectOpacity).
inline constexpr float kPendingEraseObjectOpacity = 0.5f;

// Indices of committed objects marked by an in-progress erase drag; they paint
// at kPendingEraseObjectOpacity instead of full opacity (the reference's
// pendingErase*Indices_ sets, App.cpp:5572-5628).
struct PendingEraseIndices {
    std::vector<std::size_t> images;
    std::vector<std::size_t> texts;
    std::vector<std::size_t> latexes;
    std::vector<std::size_t> strokes;

    [[nodiscard]] bool Empty() const {
        return images.empty() && texts.empty() && latexes.empty() && strokes.empty();
    }
};

// Committed objects excluded from a render, as sorted per-kind index lists
// (e.g. the object being edited, or the dragged selection while a move/
// transform gesture paints it live on top).  Excluded objects are also
// excluded from cache signatures, so mutating them does not invalidate the
// surface.
struct ExcludedIndices {
    std::vector<std::size_t> images;
    std::vector<std::size_t> texts;
    std::vector<std::size_t> latexes;
    std::vector<std::size_t> strokes;

    [[nodiscard]] bool Empty() const {
        return images.empty() && texts.empty() && latexes.empty() && strokes.empty();
    }
};

// Committed-object render parameters. `scale` maps page units to painter units;
// `devicePixelRatio` is applied only when allocating a raster surface.
struct PageRenderParams {
    float scale = 1.0f;
    float devicePixelRatio = 1.0f;
    float strokeSmoothing = 0.55f;
    float dashLength = 3.0f;
    float dashGap = 2.0f;
    // Committed-content revision of the document the page belongs to
    // (CanvasController::Revision).  Every committed mutation bumps it, and
    // in-place edits that do not (move/transform gestures mid-flight) only
    // ever touch objects the caller excludes below, so the surface signature
    // keys on this revision plus a structural fingerprint instead of hashing
    // every stroke point - O(objects) per paint instead of O(points).
    std::uint64_t contentRevision = 0;
    // Optional lazy PDF page background. When set, pages with a PDF source and
    // no stored raster draw their background from the provider at `scale`.
    PdfBackgroundProvider* pdfBackground = nullptr;
    // Committed objects excluded from this render (e.g. the object being
    // edited). Excluded objects are also excluded from cache signatures.
    ExcludedIndices excluded;
    // Objects marked by an in-progress erase drag paint at
    // kPendingEraseObjectOpacity (App.cpp:9519-9568).  Part of the surface
    // signature, like the excluded indices.
    PendingEraseIndices pendingErase;
    // Optional page-unit bounds: objects fully outside are skipped.  Used by
    // RenderPageSurfaceRegion to avoid building paths for objects that cannot
    // touch the rendered region.
    std::optional<QRectF> cullRect;
};

// Draws the committed objects of one page in layer order: page/PDF background,
// images, committed text, LaTeX, strokes. `pageOrigin` is the page's top-left
// in the painter's coordinate space; the painter's state is preserved.
void PaintPage(QPainter& painter, const Page& page, const QPointF& pageOrigin, const PageRenderParams& params);

// Draws only the listed objects of one page, in layer order, with no
// background.  Used to paint a dragged selection live on top of a cached
// surface that was rendered with those objects excluded.  `pageOrigin` and
// `params.scale` behave exactly like PaintPage.
void PaintPageSubset(QPainter& painter, const Page& page, const QPointF& pageOrigin,
    const PageRenderParams& params, const ExcludedIndices& only);

// Draws one image in page units (rotation and opacity applied). `params.scale`
// maps page units to painter units exactly like PaintPage.
void PaintImage(QPainter& painter, const PageImage& image, const PageRenderParams& params, float opacity);

// Draws one stroke in page units for a transient preview (live stroke, pending
// erase). Same pen/path/dash/opacity rules as committed strokes; `params.scale`
// maps page units to painter units exactly like PaintPage.
void PaintStrokePreview(QPainter& painter, const Stroke& stroke, const PageRenderParams& params, float opacity);

// Renders only `region` (page units) of a page into a premultiplied ARGB32
// QImage sized region * scale * devicePixelRatio, for compositing over a
// cached full-page surface.  Objects outside `region` are culled.
[[nodiscard]] QImage RenderPageSurfaceRegion(
    const Page& page, const PageRenderParams& params, const QRectF& region);

// Renders a page into a top-down premultiplied ARGB32 QImage sized
// page.width * scale * devicePixelRatio.
[[nodiscard]] QImage RenderPageSurface(const Page& page, const PageRenderParams& params);

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
