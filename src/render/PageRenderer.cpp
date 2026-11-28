#include "render/PageRenderer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include "latex/LatexEngine.h"
#include "render/PdfBackgroundProvider.h"
#include "selection/InkOutline.h"
#include "selection/StrokeSmoothing.h"
#include "text/TextLayoutQt.h"

namespace cnote::render {

namespace {

constexpr float kMinimumStrokeRenderWidth = 1.25f;
constexpr float kStrokeRenderWidthDivisor = 2.15f;
// Largest deviation of flattened ink arcs and curves, in painter units.
constexpr float kInkTolerance = 0.2f;

QColor StrokeColor(const Stroke& stroke, float opacity) {
    QColor color = QColor::fromRgba(stroke.style.color);
    color.setAlphaF(color.alphaF() * std::clamp(opacity, 0.0f, 1.0f));
    return color;
}

// Draws the slice of a lazy PDF background a region render needs.  The full
// background is the source page at the raster scale, centred on the page and
// rotated by Page::rotationDegrees (exactly like the full-page branch in
// PaintPageBackground); this maps the raster's visible rect back into that
// background's pixel space and asks the provider for only that rectangle.
// Returns false when the provider has no region path, so the caller falls back.
bool PaintPdfBackgroundRegion(QPainter& painter, const Page& page, const PageRenderParams& params,
    const QRectF& pageRect, const QRect& deviceRegion) {
    if (params.pdfBackground == nullptr || deviceRegion.isEmpty()) {
        return false;
    }
    const qreal backgroundWidth = pageRect.width();
    const qreal backgroundHeight = pageRect.height();
    if (backgroundWidth < 1.0 || backgroundHeight < 1.0) {
        return false;
    }

    // The painter origin is this raster's top-left (deviceRegion's top-left),
    // so the region whose background is wanted lands here.  For a tile of a
    // tiled page this is the whole visible page region: every tile requests the
    // same background and draws its own slice, so split glyphs are not
    // re-rasterized per tile.  (Pixels a tile covers outside that region are
    // clipped by the canvas' visible bounds.)
    const QRect backgroundRegion = params.pdfBackgroundRegion.value_or(deviceRegion);
    const QRectF visible = QRectF(backgroundRegion).translated(
        -deviceRegion.x(), -deviceRegion.y());
    const QPointF center = pageRect.center();
    const qreal radians = static_cast<qreal>(page.rotationDegrees) * (3.14159265358979323846 / 180.0);
    const qreal sine = std::sin(-radians);
    const qreal cosine = std::cos(-radians);

    qreal minX = std::numeric_limits<qreal>::max();
    qreal minY = std::numeric_limits<qreal>::max();
    qreal maxX = std::numeric_limits<qreal>::lowest();
    qreal maxY = std::numeric_limits<qreal>::lowest();
    const QPointF corners[] = { visible.topLeft(), visible.topRight(), visible.bottomRight(), visible.bottomLeft() };
    for (const QPointF& corner : corners) {
        const qreal dx = corner.x() - center.x();
        const qreal dy = corner.y() - center.y();
        const qreal localX = (dx * cosine) - (dy * sine) + (backgroundWidth * 0.5);
        const qreal localY = (dx * sine) + (dy * cosine) + (backgroundHeight * 0.5);
        minX = std::min(minX, localX);
        minY = std::min(minY, localY);
        maxX = std::max(maxX, localX);
        maxY = std::max(maxY, localY);
    }

    // Round out and pad by a pixel so the provider's own edge clipping cannot
    // shave the visible pixels, then clamp to the background.
    QRect sourceRect = QRectF(minX, minY, maxX - minX, maxY - minY)
        .toAlignedRect()
        .adjusted(-1, -1, 1, 1)
        .intersected(QRect(0, 0,
            std::max(1, static_cast<int>(std::lround(backgroundWidth))),
            std::max(1, static_cast<int>(std::lround(backgroundHeight)))));
    if (sourceRect.isEmpty()) {
        return false;
    }

    const std::optional<QImage> region = params.pdfBackground->PageBackgroundRegion(
        page.sourcePdfSourceIndex, page.sourcePdfPageIndex, std::max(0.01f, params.scale), sourceRect);
    if (!region.has_value() || region->isNull()) {
        return false;
    }

    painter.save();
    painter.translate(center);
    painter.rotate(static_cast<qreal>(page.rotationDegrees));
    // The provider's pixels are the background raster's own pixels, so the
    // slice's top-left is shifted from the image's centred top-left.
    painter.drawImage(
        QPointF(sourceRect.x() - (backgroundWidth * 0.5), sourceRect.y() - (backgroundHeight * 0.5)),
        *region);
    painter.restore();
    return true;
}

void PaintPageBackground(QPainter& painter, const Page& page, const PageRenderParams& params) {
    const float scale = std::max(0.01f, params.scale);
    const QRectF pageRect(0.0, 0.0, static_cast<qreal>(page.width * scale), static_cast<qreal>(page.height * scale));
    painter.fillRect(pageRect, Qt::white);
    if (!page.raster.pixels.empty() && page.raster.width > 0 && page.raster.height > 0) {
        const QImage view = WrapBgraImage(
            page.raster.pixels.data(),
            page.raster.width,
            page.raster.height,
            page.raster.stride);
        painter.drawImage(pageRect, view);
        return;
    }
    if (params.pdfBackground != nullptr && page.sourcePdfSourceIndex >= 0 && page.sourcePdfPageIndex >= 0) {
        if (params.deviceRegion.has_value()
            && PaintPdfBackgroundRegion(painter, page, params, pageRect, *params.deviceRegion)) {
            return;
        }
        const QImage background = params.pdfBackground->PageBackground(
            page.sourcePdfSourceIndex,
            page.sourcePdfPageIndex,
            scale);
        if (!background.isNull()) {
            // The source page renders at its stored orientation; page
            // rotation applies about the (already swapped) page centre.
            painter.save();
            painter.translate(pageRect.center());
            painter.rotate(static_cast<qreal>(page.rotationDegrees));
            painter.drawImage(
                QRectF(-static_cast<qreal>(background.width()) * 0.5, -static_cast<qreal>(background.height()) * 0.5,
                    static_cast<qreal>(background.width()), static_cast<qreal>(background.height())),
                background);
            painter.restore();
        }
    }
}


void PaintText(QPainter& painter, const PageText& text, float pageWidth, float scale,
    std::optional<float> pixelRatio, float opacity) {
    // No ratio: lay out in page units (layout scale 1).
    const float ratio = pixelRatio.value_or(scale);
    if (std::abs(text.rotationDegrees) > 0.0001f) {
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, pageWidth);
        const qreal centerX = static_cast<qreal>(bounds.left + bounds.right) * 0.5 * scale;
        const qreal centerY = static_cast<qreal>(bounds.top + bounds.bottom) * 0.5 * scale;
        painter.save();
        painter.translate(centerX, centerY);
        painter.rotate(static_cast<qreal>(text.rotationDegrees));
        painter.translate(-centerX, -centerY);
        text::DrawPageTextQt(painter, text, pageWidth, scale, ratio, text.x * scale, text.y * scale, opacity);
        painter.restore();
        return;
    }
    text::DrawPageTextQt(painter, text, pageWidth, scale, ratio, text.x * scale, text.y * scale, opacity);
}

void PaintLatex(QPainter& painter, const PageLatex& latex, float scale, float opacity) {
    const latex::FormulaHandle formula = latex::Parse(latex);
    if (formula == nullptr) {
        return;
    }
    const float clampedOpacity = std::clamp(opacity, 0.0f, 1.0f);
    const std::uint32_t alpha = static_cast<std::uint32_t>(
        std::lround(static_cast<float>((latex.color >> 24U) & 0xFFU) * clampedOpacity));
    const std::uint32_t color = (latex.color & 0x00FFFFFFU) | (alpha << 24U);
    if (std::abs(latex.rotationDegrees) > 0.0001f) {
        const latex::FormulaMetrics metrics = latex::Measure(formula);
        const qreal centerX = (static_cast<qreal>(latex.x) + static_cast<qreal>(metrics.width) * 0.5) * scale;
        const qreal centerY = (static_cast<qreal>(latex.y) + static_cast<qreal>(metrics.height) * 0.5) * scale;
        painter.save();
        painter.translate(centerX, centerY);
        painter.rotate(static_cast<qreal>(latex.rotationDegrees));
        painter.translate(-centerX, -centerY);
        latex::Draw(formula, painter, latex.x * scale, latex.y * scale, scale, color);
        painter.restore();
        return;
    }
    latex::Draw(formula, painter, latex.x * scale, latex.y * scale, scale, color);
}


std::uint64_t HashValue(std::uint64_t hash, std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
    return hash;
}

std::uint64_t HashFloat(std::uint64_t hash, float value) {
    return HashValue(hash, std::bit_cast<std::uint32_t>(value));
}

}  // namespace

namespace {

// Conservative page-unit bounds used by the cullRect fast path: objects whose
// work.  Stroke bounds are inflated by the pen's half width.
bool StrokeIntersectsCull(const Stroke& stroke, const QRectF& cull) {
    if (stroke.points.empty()) {
        return false;
    }
    const float margin = stroke.style.width * 0.5f + 1.0f;
    float minX = stroke.points.front().x;
    float minY = stroke.points.front().y;
    float maxX = minX;
    float maxY = minY;
    for (const Vec2& point : stroke.points) {
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
        maxX = std::max(maxX, point.x);
        maxY = std::max(maxY, point.y);
    }
    return QRectF(minX - margin, minY - margin,
        (maxX - minX) + margin * 2.0, (maxY - minY) + margin * 2.0).intersects(cull);
}

bool ImageIntersectsCull(const PageImage& image, const QRectF& cull) {
    // Rotated bounds: transform the four corners about the centre.
    const float radians = image.rotationDegrees * (3.14159265358979323846f / 180.0f);
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    const float centerX = image.x + image.width * 0.5f;
    const float centerY = image.y + image.height * 0.5f;
    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    for (const Vec2 corner : { Vec2 { image.x, image.y }, Vec2 { image.x + image.width, image.y },
             Vec2 { image.x + image.width, image.y + image.height }, Vec2 { image.x, image.y + image.height } }) {
        const float dx = corner.x - centerX;
        const float dy = corner.y - centerY;
        const float rx = centerX + (dx * cosine) - (dy * sine);
        const float ry = centerY + (dx * sine) + (dy * cosine);
        minX = std::min(minX, rx);
        minY = std::min(minY, ry);
        maxX = std::max(maxX, rx);
        maxY = std::max(maxY, ry);
    }
    return QRectF(minX, minY, maxX - minX, maxY - minY).intersects(cull);
}

bool TextIntersectsCull(const PageText& text, float pageWidth, const QRectF& cull) {
    const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, pageWidth);
    float minX = bounds.left;
    float minY = bounds.top;
    float maxX = bounds.right;
    float maxY = bounds.bottom;
    if (std::abs(text.rotationDegrees) > 0.0001f) {
        const float radians = text.rotationDegrees * (3.14159265358979323846f / 180.0f);
        const float sine = std::sin(radians);
        const float cosine = std::cos(radians);
        const float centerX = (bounds.left + bounds.right) * 0.5f;
        const float centerY = (bounds.top + bounds.bottom) * 0.5f;
        minX = std::numeric_limits<float>::max();
        minY = std::numeric_limits<float>::max();
        maxX = std::numeric_limits<float>::lowest();
        maxY = std::numeric_limits<float>::lowest();
        for (const Vec2 corner : { Vec2 { bounds.left, bounds.top }, Vec2 { bounds.right, bounds.top },
                 Vec2 { bounds.right, bounds.bottom }, Vec2 { bounds.left, bounds.bottom } }) {
            const float dx = corner.x - centerX;
            const float dy = corner.y - centerY;
            minX = std::min(minX, centerX + (dx * cosine) - (dy * sine));
            minY = std::min(minY, centerY + (dx * sine) + (dy * cosine));
            maxX = std::max(maxX, centerX + (dx * cosine) - (dy * sine));
            maxY = std::max(maxY, centerY + (dx * sine) + (dy * cosine));
        }
    }
    return QRectF(minX, minY, maxX - minX, maxY - minY).intersects(cull);
}

bool LatexIntersectsCull(const PageLatex& latex, const QRectF& cull) {
    const latex::FormulaMetrics metrics = latex::Measure(latex::Parse(latex));
    float minX = latex.x;
    float minY = latex.y;
    float maxX = latex.x + metrics.width;
    float maxY = latex.y + metrics.height;
    if (std::abs(latex.rotationDegrees) > 0.0001f) {
        const float radians = latex.rotationDegrees * (3.14159265358979323846f / 180.0f);
        const float sine = std::sin(radians);
        const float cosine = std::cos(radians);
        const float centerX = (minX + maxX) * 0.5f;
        const float centerY = (minY + maxY) * 0.5f;
        float outMinX = std::numeric_limits<float>::max();
        float outMinY = std::numeric_limits<float>::max();
        float outMaxX = std::numeric_limits<float>::lowest();
        float outMaxY = std::numeric_limits<float>::lowest();
        for (const Vec2 corner : { Vec2 { minX, minY }, Vec2 { maxX, minY }, Vec2 { maxX, maxY }, Vec2 { minX, maxY } }) {
            const float dx = corner.x - centerX;
            const float dy = corner.y - centerY;
            outMinX = std::min(outMinX, centerX + (dx * cosine) - (dy * sine));
            outMinY = std::min(outMinY, centerY + (dx * sine) + (dy * cosine));
            outMaxX = std::max(outMaxX, centerX + (dx * cosine) - (dy * sine));
            outMaxY = std::max(outMaxY, centerY + (dx * sine) + (dy * cosine));
        }
        minX = outMinX;
        minY = outMinY;
        maxX = outMaxX;
        maxY = outMaxY;
    }
    return QRectF(minX, minY, maxX - minX, maxY - minY).intersects(cull);
}

}  // namespace

void PaintPageText(QPainter& painter, const PageText& text, float pageWidth, float scale,
    std::optional<float> pixelRatio, float opacity) {
    PaintText(painter, text, pageWidth, scale, pixelRatio, opacity);
}

void PaintPageLatex(QPainter& painter, const PageLatex& latex, float scale, float opacity) {
    PaintLatex(painter, latex, scale, opacity);
}


void PaintImage(QPainter& painter, const PageImage& image, const PageRenderParams& params, float opacity) {
    if (image.pixels.empty() || image.pixelWidth <= 0 || image.pixelHeight <= 0) {
        return;
    }

    const float scale = std::max(0.01f, params.scale);
    const QImage view = WrapBgraImage(image.pixels.data(), image.pixelWidth, image.pixelHeight, image.stride);
    const qreal drawWidth = static_cast<qreal>(image.width * scale);
    const qreal drawHeight = static_cast<qreal>(image.height * scale);
    const qreal centerX = static_cast<qreal>((image.x + (image.width * 0.5f)) * scale);
    const qreal centerY = static_cast<qreal>((image.y + (image.height * 0.5f)) * scale);

    painter.save();
    painter.setOpacity(painter.opacity() * std::clamp(opacity, 0.0f, 1.0f));
    // Filter parity: the legacy page-cache renderer draws page objects with
    // GDI+ InterpolationModeHighQualityBicubic / PixelOffsetModeHighQuality /
    // CompositingQualityHighQuality (src/App.cpp:4380), while the transient
    // interaction previews use InterpolationModeBilinear (src/App.cpp:7282).
    // Qt's SmoothPixmapTransform is a bilinear upscale, so committed page images
    // match the reference's *direction* (measured on the frozen reference:
    // blended quadrant centres, gradient scanline - nearest-neighbour produced
    // hard quadrants and was wrong) but not its kernel: the reference smooths
    // further (69-run scanline and pulled-in corners on the 2x2 test image,
    // Qt 21 runs with pure corners at 175%). Closing that would need a bicubic
    // resample of page images; plan 3.3 does not pin the filter, so it stays an
    // open fidelity note. Only the cached page-surface blit turns smoothing off
    // (it is a 1:1 device-pixel copy).
    painter.translate(centerX, centerY);
    painter.rotate(static_cast<qreal>(image.rotationDegrees));
    painter.drawImage(QRectF(-drawWidth * 0.5, -drawHeight * 0.5, drawWidth, drawHeight), view);
    painter.restore();
}

float StrokeRenderPenWidth(const Stroke& stroke, float scale) {
    return std::max(kMinimumStrokeRenderWidth, stroke.style.width * scale / kStrokeRenderWidthDivisor);
}

float StrokeRenderInkDiameter(const Stroke& stroke, float scale, float pressure) {
    return std::max(kMinimumStrokeRenderWidth,
        stroke.style.width * scale / kStrokeRenderWidthDivisor * InkWidthFactor(pressure));
}

QPainterPath InkOutlinePath(const InkOutline& outline) {
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    std::uint32_t begin = 0;
    for (const std::uint32_t end : outline.polygonEnds) {
        if (end > begin) {
            const Vec2 first = outline.vertices[begin];
            path.moveTo(static_cast<qreal>(first.x), static_cast<qreal>(first.y));
            for (std::uint32_t index = begin + 1U; index < end; ++index) {
                path.lineTo(static_cast<qreal>(outline.vertices[index].x), static_cast<qreal>(outline.vertices[index].y));
            }
            path.closeSubpath();
        }
        begin = end;
    }
    return path;
}

void PaintStrokePreview(QPainter& painter, const Stroke& stroke, const PageRenderParams& params, float opacity) {
    if (stroke.points.empty()) {
        return;
    }

    const std::shared_ptr<const InkPath> ink = CachedStrokeRenderPath(stroke, params.strokeSmoothing, true);
    const std::vector<Vec2>& points = ink->points;
    const float scale = params.scale;
    const QColor color = StrokeColor(stroke, opacity);

    if (StrokeHasPressure(stroke)) {
        InkOutline outline;
        AppendInkOutline(outline,
            BuildInkCenterline(*ink, scale,
                [&stroke, scale](float pressure) { return StrokeRenderInkDiameter(stroke, scale, pressure); },
                kInkTolerance),
            kInkTolerance);
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.fillPath(InkOutlinePath(outline), color);
        painter.restore();
        return;
    }

    const float penWidth = StrokeRenderPenWidth(stroke, scale);

    QPen pen(color, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    if (stroke.style.dotted) {
        pen.setStyle(Qt::CustomDashLine);
        pen.setDashPattern({
            static_cast<qreal>(std::max(0.5f, params.dashLength)),
            static_cast<qreal>(std::max(0.5f, params.dashGap)),
        });
        pen.setCapStyle(Qt::RoundCap);
    }

    painter.save();
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    if (points.size() == 1U) {
        // Fill only: leaving the pen active would stroke the ellipse outline
        // with the full pen width and double the dot's diameter.
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const qreal radius = static_cast<qreal>(penWidth * 0.5f);
        painter.drawEllipse(
            QPointF(static_cast<qreal>(points.front().x * scale), static_cast<qreal>(points.front().y * scale)),
            radius,
            radius);
    } else if (points.size() == 2U) {
        painter.drawLine(
            QPointF(static_cast<qreal>(points[0].x * scale), static_cast<qreal>(points[0].y * scale)),
            QPointF(static_cast<qreal>(points[1].x * scale), static_cast<qreal>(points[1].y * scale)));
    } else {
        QPainterPath path;
        path.moveTo(static_cast<qreal>(points.front().x * scale), static_cast<qreal>(points.front().y * scale));
        for (std::size_t index = 1; index + 1 < points.size(); ++index) {
            const QPointF control(
                static_cast<qreal>(points[index].x * scale),
                static_cast<qreal>(points[index].y * scale));
            const QPointF midpoint(
                static_cast<qreal>((points[index].x + points[index + 1].x) * 0.5f * scale),
                static_cast<qreal>((points[index].y + points[index + 1].y) * 0.5f * scale));
            path.quadTo(control, midpoint);
        }
        path.lineTo(
            static_cast<qreal>(points.back().x * scale),
            static_cast<qreal>(points.back().y * scale));
        painter.drawPath(path);
    }
    painter.restore();
}

QImage WrapBgraImage(const std::uint8_t* pixels, int width, int height, int stride) {
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return {};
    }
    const qsizetype bytesPerLine = stride > 0 ? static_cast<qsizetype>(stride) : static_cast<qsizetype>(width) * 4;
    return QImage(pixels, width, height, bytesPerLine, QImage::Format_ARGB32);
}

void PaintPage(QPainter& painter, const Page& page, const QPointF& pageOrigin, const PageRenderParams& params) {
    const float scale = std::max(0.01f, params.scale);
    painter.save();
    painter.translate(pageOrigin);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    PaintPageBackground(painter, page, params);

    // Pending-erase objects paint faded (App.cpp:9519-9568); everything else
    // keeps full opacity.
    const auto pendingOpacity = [](const std::vector<std::size_t>& pending, std::size_t index) {
        return std::find(pending.begin(), pending.end(), index) != pending.end()
            ? kPendingEraseObjectOpacity
            : 1.0f;
    };
    // Exclusion lists are sorted, so membership is a binary search.
    const auto excluded = [](const std::vector<std::size_t>& list, std::size_t index) {
        return std::binary_search(list.begin(), list.end(), index);
    };

    for (std::size_t index = 0; index < page.images.size(); ++index) {
        if (excluded(params.excluded.images, index)
            || (params.cullRect.has_value() && !ImageIntersectsCull(page.images[index], *params.cullRect))) {
            continue;
        }
        PaintImage(painter, page.images[index], params, pendingOpacity(params.pendingErase.images, index));
    }

    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        if (excluded(params.excluded.texts, index)
            || (params.cullRect.has_value() && !TextIntersectsCull(page.texts[index], page.width, *params.cullRect))) {
            continue;
        }
        PaintText(painter, page.texts[index], page.width, scale, params.textPixelRatio,
            pendingOpacity(params.pendingErase.texts, index));
    }

    for (std::size_t index = 0; index < page.latexes.size(); ++index) {
        if (excluded(params.excluded.latexes, index)
            || (params.cullRect.has_value() && !LatexIntersectsCull(page.latexes[index], *params.cullRect))) {
            continue;
        }
        PaintLatex(painter, page.latexes[index], scale, pendingOpacity(params.pendingErase.latexes, index));
    }

    for (std::size_t index = 0; index < page.strokes.size(); ++index) {
        if (excluded(params.excluded.strokes, index)
            || (params.cullRect.has_value() && !StrokeIntersectsCull(page.strokes[index], *params.cullRect))) {
            continue;
        }
        PaintStrokePreview(painter, page.strokes[index], params, pendingOpacity(params.pendingErase.strokes, index));
    }

    painter.restore();
}

void PaintPageSubset(QPainter& painter, const Page& page, const QPointF& pageOrigin,
    const PageRenderParams& params, const ExcludedIndices& only) {
    const float scale = std::max(0.01f, params.scale);
    painter.save();
    painter.translate(pageOrigin);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Same layer order as PaintPage so a dragged selection composites like it
    // does when committed.
    for (const std::size_t index : only.images) {
        if (index < page.images.size()) {
            PaintImage(painter, page.images[index], params, 1.0f);
        }
    }
    for (const std::size_t index : only.texts) {
        if (index < page.texts.size()) {
            PaintText(painter, page.texts[index], page.width, scale, params.textPixelRatio, 1.0f);
        }
    }
    for (const std::size_t index : only.latexes) {
        if (index < page.latexes.size()) {
            PaintLatex(painter, page.latexes[index], scale, 1.0f);
        }
    }
    for (const std::size_t index : only.strokes) {
        if (index < page.strokes.size()) {
            PaintStrokePreview(painter, page.strokes[index], params, 1.0f);
        }
    }
    painter.restore();
}

int RegionAlignment(const PageRenderParams& params) {
    const float devicePixelRatio = std::max(0.01f, params.devicePixelRatio);
    const double ratio = params.textPixelRatio.has_value()
        ? static_cast<double>(*params.textPixelRatio) * static_cast<double>(devicePixelRatio)
        : static_cast<double>(std::max(0.01f, params.scale)) * static_cast<double>(devicePixelRatio);
    // Smallest numerator of a rational approximation whose error is far below
    // one 26.6 subpixel step.  Real display ratios (1, 1.25, 1.5, 1.75, 2, 2.5)
    // all resolve with a denominator <= 4.
    for (int denominator = 1; denominator <= 16; ++denominator) {
        const long long numerator = std::llround(ratio * denominator);
        if (numerator >= 1 && numerator <= 4096
            && std::abs(static_cast<double>(numerator) / denominator - ratio) < 1.0e-3) {
            return static_cast<int>(numerator);
        }
    }
    return 1;
}

int PageTileSize(const PageRenderParams& params) {
    const int alignment = RegionAlignment(params);
    const int aligned = (kPageTileSize / alignment) * alignment;
    return std::max(alignment, aligned);
}

QSize PageSurfacePixelSize(const Page& page, const PageRenderParams& params) {
    const float deviceScale = std::max(0.01f, params.scale * params.devicePixelRatio);
    // Saturate instead of casting an out-of-range double to int (undefined).
    constexpr double kSizeLimit = 1.0e7;
    const auto roundSize = [](double value) {
        return std::max(1, static_cast<int>(std::min(std::lround(value), static_cast<long>(kSizeLimit))));
    };
    return QSize(roundSize(static_cast<double>(page.width) * static_cast<double>(deviceScale)),
        roundSize(static_cast<double>(page.height) * static_cast<double>(deviceScale)));
}

namespace {

PageRenderParams DeviceParams(const PageRenderParams& params) {
    PageRenderParams deviceParams = params;
    deviceParams.scale = std::max(0.01f, params.scale * params.devicePixelRatio);
    deviceParams.devicePixelRatio = 1.0f;
    if (params.textPixelRatio.has_value()) {
        deviceParams.textPixelRatio = *params.textPixelRatio * params.devicePixelRatio;
    }
    return deviceParams;
}

QImage RenderPageSurfaceOfSize(const Page& page, const PageRenderParams& params, const QSize& size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::transparent);
    QPainter painter(&image);
    PageRenderParams deviceParams = DeviceParams(params);
    deviceParams.deviceRegion.reset();
    PaintPage(painter, page, QPointF(0.0, 0.0), deviceParams);
    painter.end();
    return image;
}

}  // namespace

QImage RenderPageSurface(const Page& page, const PageRenderParams& params, RasterBudget budget) {
    const QSize size = PageSurfacePixelSize(page, params);
    if (!RasterFitsBudget(size.width(), size.height(), budget)) {
        return {};
    }
    return RenderPageSurfaceOfSize(page, params, size);
}

QImage RenderPageSurfaceRegion(const Page& page, const PageRenderParams& params, const QRectF& region,
    RasterBudget budget) {
    const float deviceScale = std::max(0.01f, params.scale * params.devicePixelRatio);
    const QRectF clipped = region.intersected(QRectF(0.0, 0.0, page.width, page.height));
    if (clipped.isEmpty()) {
        return {};
    }
    const std::optional<QSize> size = CheckedRasterSize(
        clipped.width() * static_cast<double>(deviceScale),
        clipped.height() * static_cast<double>(deviceScale), budget);
    if (!size.has_value()) {
        return {};
    }

    QImage image(*size, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::transparent);

    QPainter painter(&image);
    PageRenderParams deviceParams = DeviceParams(params);
    deviceParams.cullRect = clipped;
    deviceParams.deviceRegion = QRect(
        static_cast<int>(std::lround(clipped.x() * deviceScale)),
        static_cast<int>(std::lround(clipped.y() * deviceScale)),
        size->width(), size->height());
    // The image's origin is the region's top-left: shift the page so the
    // region lands at (0,0) in device pixels.
    PaintPage(painter, page, QPointF(-clipped.x() * deviceScale, -clipped.y() * deviceScale), deviceParams);
    painter.end();
    return image;
}

QImage RenderPageSurfaceRegionPixels(const Page& page, const PageRenderParams& params, const QRect& deviceRect,
    RasterBudget budget) {
    const QSize pageSize = PageSurfacePixelSize(page, params);
    const QRect clipped = deviceRect.intersected(QRect(QPoint(0, 0), pageSize));
    if (clipped.isEmpty() || !RasterFitsBudget(clipped.width(), clipped.height(), budget)) {
        return {};
    }

    QImage image(clipped.size(), QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::transparent);

    QPainter painter(&image);
    PageRenderParams deviceParams = DeviceParams(params);
    const float deviceScale = deviceParams.scale;
    deviceParams.cullRect = QRectF(
        static_cast<qreal>(clipped.x()) / deviceScale,
        static_cast<qreal>(clipped.y()) / deviceScale,
        static_cast<qreal>(clipped.width()) / deviceScale,
        static_cast<qreal>(clipped.height()) / deviceScale);
    deviceParams.deviceRegion = clipped;
    // Integer translation: the tile's pixels are the full surface's pixels at
    // the same device coordinates, so adjacent tiles join without seams.
    PaintPage(painter, page, QPointF(-clipped.x(), -clipped.y()), deviceParams);
    painter.end();
    return image;
}
std::uint64_t PageSurfaceSignature(const Page& page, const PageRenderParams& params) {
    // Signature contract: the cached surface is re-rendered when this value
    // changes.  Committed mutations all bump params.contentRevision, so the
    // content part is a structural fingerprint - container sizes and storage
    // identities plus per-object headers - instead of a deep hash of every
    // stroke point.  In-place edits that bypass the revision (mid-gesture
    // moves/transforms) only ever touch objects the caller lists in
    // params.excluded, which stay out of the fingerprint exactly like they
    // stayed out of the old deep hash.
    std::uint64_t hash = 14695981039346656037ULL;
    hash = HashFloat(hash, page.width);
    hash = HashFloat(hash, page.height);
    hash = HashFloat(hash, params.scale);
    hash = HashFloat(hash, params.devicePixelRatio);
    hash = HashFloat(hash, params.textPixelRatio.value_or(0.0f));
    hash = HashFloat(hash, params.strokeSmoothing);
    hash = HashFloat(hash, params.dashLength);
    hash = HashFloat(hash, params.dashGap);
    hash = HashValue(hash, params.contentRevision);
    // Exclusion lists are part of the signature; excluded objects' content is
    // not, so mutating an excluded object (a dragged selection, the object
    // under edit) does not invalidate the surface.
    const auto hashIndices = [&hash](const std::vector<std::size_t>& list) {
        hash = HashValue(hash, list.size());
        for (const std::size_t index : list) {
            hash = HashValue(hash, index);
        }
    };
    hashIndices(params.excluded.images);
    hashIndices(params.excluded.texts);
    hashIndices(params.excluded.latexes);
    hashIndices(params.excluded.strokes);
    const auto excluded = [](const std::vector<std::size_t>& list, std::size_t index) {
        return std::binary_search(list.begin(), list.end(), index);
    };
    const auto hashPending = [&hash](const std::vector<std::size_t>& pending) {
        hash = HashValue(hash, pending.size());
        for (const std::size_t index : pending) {
            hash = HashValue(hash, index);
        }
    };
    hashPending(params.pendingErase.images);
    hashPending(params.pendingErase.texts);
    hashPending(params.pendingErase.latexes);
    hashPending(params.pendingErase.strokes);

    // Storage identities pin the page instance and its containers, so a fresh
    // document at the same revision (load, attach, undo of everything) cannot
    // collide with a previous page's cached signature.
    hash = HashValue(hash, page.raster.width);
    hash = HashValue(hash, page.raster.height);
    hash = HashValue(hash, page.raster.stride);
    hash = HashValue(hash, page.raster.pixels.size());
    hash = HashValue(hash, reinterpret_cast<std::uintptr_t>(page.raster.pixels.data()));
    hash = HashFloat(hash, page.rotationDegrees);
    // PDF source identity is part of the painted background: retargeting a
    // page to another source or source page must invalidate its surface.
    hash = HashValue(hash, static_cast<std::uint32_t>(page.sourcePdfSourceIndex));
    hash = HashValue(hash, static_cast<std::uint32_t>(page.sourcePdfPageIndex));
    if (params.pdfBackground != nullptr) {
        hash = HashValue(hash, reinterpret_cast<std::uintptr_t>(params.pdfBackground));
    }

    hash = HashValue(hash, page.images.size());
    hash = HashValue(hash, reinterpret_cast<std::uintptr_t>(page.images.data()));
    for (std::size_t index = 0; index < page.images.size(); ++index) {
        if (excluded(params.excluded.images, index)) {
            continue;
        }
        const PageImage& image = page.images[index];
        hash = HashFloat(hash, image.x);
        hash = HashFloat(hash, image.y);
        hash = HashFloat(hash, image.width);
        hash = HashFloat(hash, image.height);
        hash = HashFloat(hash, image.rotationDegrees);
        hash = HashValue(hash, image.pixelWidth);
        hash = HashValue(hash, image.pixelHeight);
        hash = HashValue(hash, image.stride);
        hash = HashValue(hash, image.pixels.size());
        hash = HashValue(hash, reinterpret_cast<std::uintptr_t>(image.pixels.data()));
    }

    hash = HashValue(hash, page.texts.size());
    hash = HashValue(hash, reinterpret_cast<std::uintptr_t>(page.texts.data()));
    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        if (excluded(params.excluded.texts, index)) {
            continue;
        }
        const PageText& text = page.texts[index];
        hash = HashValue(hash, text::TextLayoutSignature(text));
        hash = HashFloat(hash, text.x);
        hash = HashFloat(hash, text.y);
        hash = HashFloat(hash, text.width);
        hash = HashFloat(hash, text.rotationDegrees);
    }

    hash = HashValue(hash, page.latexes.size());
    hash = HashValue(hash, reinterpret_cast<std::uintptr_t>(page.latexes.data()));
    for (std::size_t index = 0; index < page.latexes.size(); ++index) {
        if (excluded(params.excluded.latexes, index)) {
            continue;
        }
        const PageLatex& latex = page.latexes[index];
        hash = HashFloat(hash, latex.x);
        hash = HashFloat(hash, latex.y);
        hash = HashFloat(hash, latex.fontSize);
        hash = HashValue(hash, latex.color);
        hash = HashFloat(hash, latex.rotationDegrees);
        for (char16_t unit : latex.content) {
            hash = HashValue(hash, static_cast<std::uint16_t>(unit));
        }
    }

    hash = HashValue(hash, page.strokes.size());
    hash = HashValue(hash, reinterpret_cast<std::uintptr_t>(page.strokes.data()));
    for (std::size_t index = 0; index < page.strokes.size(); ++index) {
        if (excluded(params.excluded.strokes, index)) {
            continue;
        }
        const Stroke& stroke = page.strokes[index];
        hash = HashValue(hash, stroke.style.color);
        hash = HashFloat(hash, stroke.style.width);
        hash = HashValue(hash, stroke.style.dotted);
        hash = HashFloat(hash, stroke.smoothing);
        hash = HashFloat(hash, stroke.sampling.streamline);
        hash = HashFloat(hash, stroke.sampling.spacing);
        hash = HashValue(hash, stroke.points.size());
        hash = HashValue(hash, stroke.pressures.size());
        hash = HashValue(hash, reinterpret_cast<std::uintptr_t>(stroke.points.data()));
        if (!stroke.points.empty()) {
            hash = HashFloat(hash, stroke.points.front().x);
            hash = HashFloat(hash, stroke.points.front().y);
            hash = HashFloat(hash, stroke.points.back().x);
            hash = HashFloat(hash, stroke.points.back().y);
        }
    }

    return hash;
}

void EnsurePageSurface(PageSurfaceCache& cache, const Page& page, const PageRenderParams& params,
    RasterBudget budget) {
    const std::uint64_t signature = PageSurfaceSignature(page, params);
    if (cache.valid && cache.signature == signature && !cache.image.isNull()) {
        return;
    }
    if (cache.failed && cache.failedSignature == signature) {
        // A refused/failed allocation is not retried on every paint; the
        // previous image (if any) stays available to the caller.
        return;
    }
    const QSize size = PageSurfacePixelSize(page, params);
    if (!RasterFitsBudget(size.width(), size.height(), budget)) {
        cache.failed = true;
        cache.failedSignature = signature;
        return;
    }
    QImage image = RenderPageSurfaceOfSize(page, params, size);
    if (image.isNull()) {
        cache.failed = true;
        cache.failedSignature = signature;
        return;
    }
    cache.image = std::move(image);
    cache.signature = signature;
    cache.valid = true;
    cache.failed = false;
    cache.failedSignature = 0;
}

}  // namespace cnote::render
