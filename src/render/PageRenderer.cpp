#include "render/PageRenderer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include "latex/LatexEngine.h"
#include "render/PdfBackgroundProvider.h"
#include "selection/StrokeSmoothing.h"
#include "text/TextLayoutQt.h"

namespace cnote::render {

namespace {

constexpr float kMinimumStrokeRenderWidth = 1.25f;
constexpr float kStrokeRenderWidthDivisor = 2.15f;

QColor StrokeColor(const Stroke& stroke, float opacity) {
    QColor color = QColor::fromRgba(stroke.style.color);
    color.setAlphaF(color.alphaF() * std::clamp(opacity, 0.0f, 1.0f));
    return color;
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
        const QImage background = params.pdfBackground->PageBackground(
            page.sourcePdfSourceIndex,
            page.sourcePdfPageIndex,
            scale);
        if (!background.isNull()) {
            painter.drawImage(pageRect, background);
        }
    }
}


void PaintText(QPainter& painter, const PageText& text, float pageWidth, float scale, float opacity) {
    text::DrawPageTextQt(painter, text, pageWidth, scale, text.x * scale, text.y * scale, opacity);
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

void PaintStrokePreview(QPainter& painter, const Stroke& stroke, const PageRenderParams& params, float opacity) {
    if (stroke.points.empty()) {
        return;
    }

    const float smoothing = StrokeRenderSmoothing(stroke, params.strokeSmoothing);
    const std::vector<Vec2> points = BuildSmoothedPoints(stroke.points, smoothing);
    const float scale = params.scale;
    const float penWidth = std::max(kMinimumStrokeRenderWidth, stroke.style.width * scale / kStrokeRenderWidthDivisor);
    const QColor color = StrokeColor(stroke, opacity);

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
        if (excluded(params.excluded.images, index)) {
            continue;
        }
        PaintImage(painter, page.images[index], params, pendingOpacity(params.pendingErase.images, index));
    }

    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        if (excluded(params.excluded.texts, index)) {
            continue;
        }
        PaintText(painter, page.texts[index], page.width, scale, pendingOpacity(params.pendingErase.texts, index));
    }

    for (std::size_t index = 0; index < page.latexes.size(); ++index) {
        if (excluded(params.excluded.latexes, index)) {
            continue;
        }
        PaintLatex(painter, page.latexes[index], scale, pendingOpacity(params.pendingErase.latexes, index));
    }

    for (std::size_t index = 0; index < page.strokes.size(); ++index) {
        if (excluded(params.excluded.strokes, index)) {
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
            PaintText(painter, page.texts[index], page.width, scale, 1.0f);
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

QImage RenderPageSurface(const Page& page, const PageRenderParams& params) {
    const float deviceScale = std::max(0.01f, params.scale * params.devicePixelRatio);
    const int width = std::max(1, static_cast<int>(std::lround(page.width * deviceScale)));
    const int height = std::max(1, static_cast<int>(std::lround(page.height * deviceScale)));

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    PageRenderParams deviceParams = params;
    deviceParams.scale = deviceScale;
    deviceParams.devicePixelRatio = 1.0f;
    PaintPage(painter, page, QPointF(0.0, 0.0), deviceParams);
    painter.end();
    return image;
}
std::uint64_t PageSurfaceSignature(const Page& page, const PageRenderParams& params) {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = HashFloat(hash, page.width);
    hash = HashFloat(hash, page.height);
    hash = HashFloat(hash, params.scale);
    hash = HashFloat(hash, params.devicePixelRatio);
    hash = HashFloat(hash, params.strokeSmoothing);
    hash = HashFloat(hash, params.dashLength);
    hash = HashFloat(hash, params.dashGap);
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

    hash = HashValue(hash, page.raster.width);
    hash = HashValue(hash, page.raster.height);
    hash = HashValue(hash, page.raster.stride);
    hash = HashValue(hash, page.raster.pixels.size());
    hash = HashValue(hash, reinterpret_cast<std::uintptr_t>(page.raster.pixels.data()));

    hash = HashValue(hash, page.images.size());
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
    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        if (excluded(params.excluded.texts, index)) {
            continue;
        }
        const PageText& text = page.texts[index];
        hash = HashValue(hash, text::TextLayoutSignature(text, page.width));
        hash = HashFloat(hash, text.x);
        hash = HashFloat(hash, text.y);
        hash = HashFloat(hash, text.width);
        hash = HashFloat(hash, text.height);
    }

    hash = HashValue(hash, page.latexes.size());
    for (std::size_t index = 0; index < page.latexes.size(); ++index) {
        if (excluded(params.excluded.latexes, index)) {
            continue;
        }
        const PageLatex& latex = page.latexes[index];
        hash = HashFloat(hash, latex.x);
        hash = HashFloat(hash, latex.y);
        hash = HashFloat(hash, latex.fontSize);
        hash = HashValue(hash, latex.color);
        for (char16_t unit : latex.content) {
            hash = HashValue(hash, static_cast<std::uint16_t>(unit));
        }
    }

    hash = HashValue(hash, page.strokes.size());
    for (std::size_t index = 0; index < page.strokes.size(); ++index) {
        if (excluded(params.excluded.strokes, index)) {
            continue;
        }
        const Stroke& stroke = page.strokes[index];
        hash = HashValue(hash, stroke.style.color);
        hash = HashFloat(hash, stroke.style.width);
        hash = HashValue(hash, stroke.style.dotted);
        hash = HashFloat(hash, stroke.smoothing);
        hash = HashValue(hash, stroke.points.size());
        for (const Vec2& point : stroke.points) {
            hash = HashFloat(hash, point.x);
            hash = HashFloat(hash, point.y);
        }
    }

    return hash;
}

void EnsurePageSurface(PageSurfaceCache& cache, const Page& page, const PageRenderParams& params) {
    const std::uint64_t signature = PageSurfaceSignature(page, params);
    if (cache.valid && cache.signature == signature && !cache.image.isNull()) {
        return;
    }
    cache.image = RenderPageSurface(page, params);
    cache.signature = signature;
    cache.valid = true;
}

}  // namespace cnote::render
