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

void PaintPageBackground(QPainter& painter, const Page& page, float scale) {
    const QRectF pageRect(0.0, 0.0, static_cast<qreal>(page.width * scale), static_cast<qreal>(page.height * scale));
    painter.fillRect(pageRect, Qt::white);
    if (!page.raster.pixels.empty() && page.raster.width > 0 && page.raster.height > 0) {
        const QImage view = WrapBgraImage(
            page.raster.pixels.data(),
            page.raster.width,
            page.raster.height,
            page.raster.stride);
        painter.drawImage(pageRect, view);
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

    PaintPageBackground(painter, page, scale);

    for (const PageImage& image : page.images) {
        PaintImage(painter, image, params, 1.0f);
    }

    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        if (params.excludedTextIndex.has_value() && *params.excludedTextIndex == index) {
            continue;
        }
        PaintText(painter, page.texts[index], page.width, scale, 1.0f);
    }

    for (std::size_t index = 0; index < page.latexes.size(); ++index) {
        if (params.excludedLatexIndex.has_value() && *params.excludedLatexIndex == index) {
            continue;
        }
        PaintLatex(painter, page.latexes[index], scale, 1.0f);
    }

    for (std::size_t index = 0; index < page.strokes.size(); ++index) {
        if (params.excludedStrokeIndex.has_value() && *params.excludedStrokeIndex == index) {
            continue;
        }
        PaintStrokePreview(painter, page.strokes[index], params, 1.0f);
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

QImage RenderPageThumbnail(const Page& page, int width) {
    if (width <= 0 || page.width <= 0.0f || page.height <= 0.0f) {
        return {};
    }
    PageRenderParams params;
    params.scale = static_cast<float>(width) / page.width;
    params.devicePixelRatio = 1.0f;
    return RenderPageSurface(page, params);
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
    hash = HashValue(hash, params.excludedTextIndex.value_or(static_cast<std::size_t>(-1)));
    hash = HashValue(hash, params.excludedLatexIndex.value_or(static_cast<std::size_t>(-1)));
    hash = HashValue(hash, params.excludedStrokeIndex.value_or(static_cast<std::size_t>(-1)));

    hash = HashValue(hash, page.raster.width);
    hash = HashValue(hash, page.raster.height);
    hash = HashValue(hash, page.raster.stride);
    hash = HashValue(hash, page.raster.pixels.size());
    hash = HashValue(hash, reinterpret_cast<std::uintptr_t>(page.raster.pixels.data()));

    hash = HashValue(hash, page.images.size());
    for (const PageImage& image : page.images) {
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
    for (const PageText& text : page.texts) {
        hash = HashValue(hash, text::TextLayoutSignature(text, page.width));
        hash = HashFloat(hash, text.x);
        hash = HashFloat(hash, text.y);
        hash = HashFloat(hash, text.width);
        hash = HashFloat(hash, text.height);
    }

    hash = HashValue(hash, page.latexes.size());
    for (const PageLatex& latex : page.latexes) {
        hash = HashFloat(hash, latex.x);
        hash = HashFloat(hash, latex.y);
        hash = HashFloat(hash, latex.fontSize);
        hash = HashValue(hash, latex.color);
        for (char16_t unit : latex.content) {
            hash = HashValue(hash, static_cast<std::uint16_t>(unit));
        }
    }

    hash = HashValue(hash, page.strokes.size());
    for (const Stroke& stroke : page.strokes) {
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
