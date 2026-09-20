// Qt PDF-export overlay composition (migration plan step 3.4).
//
// The export orchestration in PdfBackend.cpp is shared; only the page raster
// composition lives here. This is the only export overlay implementation (the
// legacy GDI+ exporter was deleted at cutover).

#include "PdfOverlay.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QImage>
#include <QPainter>

#include "PdfBackend.h"
#include "latex/LatexEngine.h"
#include "render/PageRenderer.h"
#include "text/TextLayoutQt.h"

namespace cnote {

namespace {

// Converts a premultiplied ARGB32 QImage to the straight-alpha BGRA bytes the
// PDFium bitmap path expects.
RasterImage ToStraightBgra(const QImage& image, float renderScale) {
    RasterImage raster {};
    raster.width = image.width();
    raster.height = image.height();
    raster.stride = image.width() * 4;
    raster.scale = renderScale;
    raster.pixels.resize(static_cast<std::size_t>(raster.stride) * static_cast<std::size_t>(raster.height));

    for (int y = 0; y < image.height(); ++y) {
        const auto* source = reinterpret_cast<const std::uint8_t*>(image.constScanLine(y));
        std::uint8_t* destination = raster.pixels.data()
            + (static_cast<std::size_t>(y) * static_cast<std::size_t>(raster.stride));
        for (int x = 0; x < image.width(); ++x) {
            const std::uint8_t blue = source[(x * 4) + 0];
            const std::uint8_t green = source[(x * 4) + 1];
            const std::uint8_t red = source[(x * 4) + 2];
            const std::uint8_t alpha = source[(x * 4) + 3];
            if (alpha == 0) {
                destination[(x * 4) + 0] = 0;
                destination[(x * 4) + 1] = 0;
                destination[(x * 4) + 2] = 0;
                destination[(x * 4) + 3] = 0;
                continue;
            }
            destination[(x * 4) + 0] = static_cast<std::uint8_t>(std::min(255, (blue * 255) / alpha));
            destination[(x * 4) + 1] = static_cast<std::uint8_t>(std::min(255, (green * 255) / alpha));
            destination[(x * 4) + 2] = static_cast<std::uint8_t>(std::min(255, (red * 255) / alpha));
            destination[(x * 4) + 3] = alpha;
        }
    }
    return raster;
}

}  // namespace

std::optional<RasterImage> ComposeTextOverlayRaster(const Page& page, float renderScale) {
    bool hasText = false;
    for (const PageText& text : page.texts) {
        if (!text.content.empty()) {
            hasText = true;
            break;
        }
    }
    const bool hasLatex = latex::IsInitialized()
        && std::any_of(
            page.latexes.begin(),
            page.latexes.end(),
            [](const PageLatex& latex) { return !latex.content.empty(); });
    if (!hasText && !hasLatex) {
        return std::nullopt;
    }

    const int width = std::max(1, static_cast<int>(std::lround(page.width * renderScale)));
    const int height = std::max(1, static_cast<int>(std::lround(page.height * renderScale)));
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    for (const PageText& text : page.texts) {
        text::DrawPageTextQt(painter, text, page.width, renderScale, text.x * renderScale, text.y * renderScale, 1.0f);
    }
    for (const PageLatex& latexBlock : page.latexes) {
        const latex::FormulaHandle formula = latex::Parse(latexBlock);
        if (formula == nullptr) {
            continue;
        }
        latex::Draw(formula, painter, latexBlock.x * renderScale, latexBlock.y * renderScale, renderScale, latexBlock.color);
    }
    painter.end();

    return ToStraightBgra(image, renderScale);
}

RasterImage ComposePageRaster(
    const PdfiumBackend& backend,
    const Document& document,
    std::size_t pageIndex,
    float renderScale,
    float strokeSmoothing,
    float dashLength,
    float dashGap,
    bool includeStrokes) {
    const Page& page = document.PageAt(pageIndex);
    std::optional<RasterImage> pdfRaster;
    if (const Document::PdfSource* pdfSource = document.PdfSourceAtIndex(page.sourcePdfSourceIndex);
        pdfSource != nullptr && page.sourcePdfPageIndex >= 0) {
        pdfRaster = backend.RenderPage(pdfSource->pdfData, static_cast<std::size_t>(page.sourcePdfPageIndex), renderScale);
    }

    const bool useStoredRaster = !pdfRaster.has_value()
        && !page.raster.pixels.empty()
        && page.raster.width > 0
        && page.raster.height > 0;
    RasterImage raster = pdfRaster.value_or(CreateBlankRaster(page.width, page.height, renderScale));

    QImage image(raster.width, raster.height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (useStoredRaster) {
        const QImage view = render::WrapBgraImage(
            page.raster.pixels.data(),
            page.raster.width,
            page.raster.height,
            page.raster.stride);
        painter.drawImage(QRectF(0.0, 0.0, raster.width, raster.height), view);
    }

    render::PageRenderParams params;
    params.scale = renderScale;
    params.strokeSmoothing = strokeSmoothing;
    params.dashLength = dashLength;
    params.dashGap = dashGap;
    for (const PageImage& imageObject : page.images) {
        render::PaintImage(painter, imageObject, params, 1.0f);
    }
    if (includeStrokes) {
        for (const Stroke& stroke : page.strokes) {
            render::PaintStrokePreview(painter, stroke, params, 1.0f);
        }
    }
    painter.end();

    return ToStraightBgra(image, renderScale);
}

}  // namespace cnote
