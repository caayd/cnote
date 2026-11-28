#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QRect>
#include <QRectF>

#include "Document.h"
#include "render/RasterBudget.h"
#include "selection/InkOutline.h"

class QPainter;

namespace cnote::render {

class PdfBackgroundProvider;

// Opacity committed objects render at while the eraser drag has marked them
// for deletion (App.cpp:172 kPendingEraseObjectOpacity).
inline constexpr float kPendingEraseObjectOpacity = 0.5f;

// Indices of committed objects marked by an in-progress erase drag; they paint
// at kPendingEraseObjectOpacity instead of full opacity (the reference's
// pendingErase*Indices_ sets, App.cpp:5572-5628).  Each list is sorted
// ascending: opacity membership is a binary search, and the order is part of
// the surface signature.
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
    // Painter units per text layout pixel.  When set, committed text is laid
    // out at scale / textPixelRatio (fonts and wrap width multiplied by it,
    // exactly like the live editor at that zoom) and the painter scales the
    // layout by exactly this ratio; Qt rounds font pixel sizes, so this is
    // what keeps committed glyphs where the editor drew them.  The canvas
    // sets 1 (logical-pixel painter); raster surfaces multiply in
    // devicePixelRatio as they fold it into `scale`.  Unset lays text out in
    // page units, like the hit-test geometry and PDF export.
    std::optional<float> textPixelRatio;
    float strokeSmoothing = 0.55f;
    float dashLength = 3.0f;
    float dashGap = 2.0f;
    // Per-page content epoch (Document::PageEpoch): every committed mutation
    // of THIS page bumps it, so commits elsewhere never invalidate this
    // surface.  In-place edits that do not (move/transform gestures
    // mid-flight) only ever touch objects the caller excludes below, so the
    // surface signature keys on this epoch plus a structural fingerprint
    // instead of hashing every stroke point - O(objects) per paint instead
    // of O(points).
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
    // Device-pixel rect of the full-page raster a region render covers, in the
    // same top-down space as RenderPageSurface.  Set by the region renderers so
    // a lazy PDF background is requested for just this region instead of
    // allocating the whole page; not part of the surface signature.
    std::optional<QRect> deviceRegion;
    // Optional larger device-pixel rect (containing `deviceRegion`) whose PDF
    // background is rendered once and sliced: every tile of a tiled page passes
    // the same rect, so a glyph crossing a tile boundary comes from one PDFium
    // render and the tiles cannot seam.  Defaults to `deviceRegion`.
    std::optional<QRect> pdfBackgroundRegion;
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

// Pen width in scaled units for a stroke rendered at `scale` (the minimum
// keeps thin strokes visible when zoomed out).  Shared by the committed
// render, the transient preview and the canvas' incremental live-stroke
// layer so all three agree on thickness.
[[nodiscard]] float StrokeRenderPenWidth(const Stroke& stroke, float scale);

// Ink diameter in scaled units for a pressure stroke at `pressure`
// (InkWidthFactor of the nominal pen width, same minimum).  Shared by the
// committed render and the live layer.
[[nodiscard]] float StrokeRenderInkDiameter(const Stroke& stroke, float scale, float pressure);

// A nonzero-filled QPainterPath of an ink outline (selection/InkOutline.h).
[[nodiscard]] QPainterPath InkOutlinePath(const InkOutline& outline);

// Draws one image in page units (rotation and opacity applied). `params.scale`
// maps page units to painter units exactly like PaintPage.
void PaintImage(QPainter& painter, const PageImage& image, const PageRenderParams& params, float opacity);

// Draws one stroke in page units for a transient preview (live stroke, pending
// erase). Same pen/path/dash/opacity rules as committed strokes, and the same
// filled outline for pressure strokes; `params.scale` maps page units to
// painter units exactly like PaintPage.
void PaintStrokePreview(QPainter& painter, const Stroke& stroke, const PageRenderParams& params, float opacity);

// Draws one text/LaTeX block in page units (rotation and opacity applied),
// same transform rules as the committed render; `pixelRatio` is
// PageRenderParams::textPixelRatio (nullopt lays out in page units).
// Exported so the PDF export text overlay reuses the rotation-aware path.
void PaintPageText(QPainter& painter, const PageText& text, float pageWidth, float scale,
    std::optional<float> pixelRatio, float opacity);
void PaintPageLatex(QPainter& painter, const PageLatex& latex, float scale, float opacity);

// Renders only `region` (page units) of a page into a premultiplied ARGB32
// QImage sized region * scale * devicePixelRatio, for compositing over a
// cached full-page surface.  Objects outside `region` are culled.  Empty when
// the region cannot be mapped to a raster inside `budget`.
[[nodiscard]] QImage RenderPageSurfaceRegion(
    const Page& page, const PageRenderParams& params, const QRectF& region,
    RasterBudget budget = RasterBudget {});

// Device-pixel size of the full-page raster RenderPageSurface allocates for
// these parameters (the same rounding RenderPageSurface uses).
[[nodiscard]] QSize PageSurfacePixelSize(const Page& page, const PageRenderParams& params);

// Device-pixel origin quantum for a region render that must be pixel-identical
// to the full-page raster.  Qt derives a glyph's horizontal subpixel phase from
// the painter translation relative to the text painter's scale, so a region
// whose origin is not a multiple of the ratio's numerator re-rasterizes text
// with a 1/64-pixel offset.  Tiles built by the canvas and by tests use this
// alignment; origins that are multiples of it reproduce the full raster's text
// exactly (and strokes/images are translation-invariant).
[[nodiscard]] int RegionAlignment(const PageRenderParams& params);

// Device-pixel size of one page tile: kPageTileSize rounded down to an integer
// multiple of RegionAlignment (never below the alignment itself).
[[nodiscard]] int PageTileSize(const PageRenderParams& params);

// Renders only `deviceRect` (device pixels, top-down, in the full-page raster's
// space) into a raster exactly that size.  A rect whose origin is a multiple of
// RegionAlignment reproduces RenderPageSurface's pixels exactly, so tiling a
// page and compositing the tiles is seamless.
[[nodiscard]] QImage RenderPageSurfaceRegionPixels(
    const Page& page, const PageRenderParams& params, const QRect& deviceRect,
    RasterBudget budget = RasterBudget {});

// Renders a page into a top-down premultiplied ARGB32 QImage sized
// page.width * scale * devicePixelRatio.  Empty when that size exceeds `budget`.
[[nodiscard]] QImage RenderPageSurface(
    const Page& page, const PageRenderParams& params, RasterBudget budget = RasterBudget {});

// Cached page surface owned by the caller. EnsurePageSurface re-renders only
// when the signature changes.
struct PageSurfaceCache {
    QImage image;
    std::uint64_t signature = 0;
    bool valid = false;
    // Signature whose render was refused (over budget) or failed to allocate.
    // Retrying it on every paint would stall the GUI thread for a surface that
    // cannot exist until the signature changes, so it is skipped; the previous
    // image (if any) stays usable.
    std::uint64_t failedSignature = 0;
    bool failed = false;
};

[[nodiscard]] std::uint64_t PageSurfaceSignature(const Page& page, const PageRenderParams& params);
void EnsurePageSurface(PageSurfaceCache& cache, const Page& page, const PageRenderParams& params,
    RasterBudget budget = RasterBudget {});

// Wraps top-down straight-alpha BGRA bytes (model/PDFium layout) as a
// non-owning ARGB32 QImage view. The caller must keep `pixels` alive for as
// long as the returned image is used.
[[nodiscard]] QImage WrapBgraImage(const std::uint8_t* pixels, int width, int height, int stride);

}  // namespace cnote::render
