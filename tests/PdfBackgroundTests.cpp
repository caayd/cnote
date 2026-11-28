// Self-contained lazy PDF-background provider test (migration plan step 3.4).
//
// Proves the Qt canvas gets PDF page backgrounds lazily: a cold open renders no
// pages, only requested pages are rendered, results are cached per raster scale
// (zoom re-renders), and the cache stays bounded while panning a 50-page PDF.
// Also proves small region patches (erase previews, move-gesture vacated
// regions, tiles) are exact crops of the page background the surface blitted:
// no extra PDFium render when a full-page raster is warm, one at most when it
// is cold, and only a bounded region above the full-page raster budget.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QGuiApplication>
#include <QImage>

#include "Document.h"
#include "PdfBackend.h"
#include "render/PageRenderer.h"
#include "render/PdfiumBackgroundProvider.h"

namespace {

int g_failures = 0;

double NowMilliseconds() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

std::vector<std::uint8_t> BuildMultiPagePdf(int pageCount, bool spreadText = false) {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    std::string kids;
    for (int index = 0; index < pageCount; ++index) {
        kids += std::to_string(4 + (index * 2)) + " 0 R ";
    }
    objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageCount) + " >>");
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    for (int index = 0; index < pageCount; ++index) {
        // `spreadText` puts a text run in three corners/centre so a region
        // parity test cannot pass on blank paper alone.
        const std::string content = spreadText
            ? "BT /F1 24 Tf 72 720 Td (Top) Tj ET\n"
              "BT /F1 24 Tf 250 400 Td (Centre) Tj ET\n"
              "BT /F1 24 Tf 400 90 Td (Bottom) Tj ET\n"
            : "BT /F1 24 Tf 72 720 Td (Page " + std::to_string(index + 1) + ") Tj ET\n";
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents "
            + std::to_string(5 + (index * 2)) + " 0 R /Resources << /Font << /F1 3 0 R >> >> >>");
        objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream");
    }

    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets(1, 0);
    for (std::size_t index = 0; index < objects.size(); ++index) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(index + 1U) + " 0 obj\n" + objects[index] + "\nendobj\n";
    }
    const std::size_t xrefOffset = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1U) + "\n0000000000 65535 f \n";
    for (std::size_t index = 1; index < offsets.size(); ++index) {
        std::string offset = std::to_string(offsets[index]);
        pdf += std::string(10U - offset.size(), '0') + offset + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1U) + " /Root 1 0 R >>\nstartxref\n";
    pdf += std::to_string(xrefOffset) + "\n%%EOF\n";
    return std::vector<std::uint8_t>(pdf.begin(), pdf.end());
}

bool HasNonWhitePixel(const QImage& image) {
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4) {
            const std::uint32_t pixel = image.pixel(x, y) & 0x00FFFFFFU;
            if (pixel != 0x00FFFFFFU) {
                return true;
            }
        }
    }
    return false;
}

// True when `actual` equals `rect` of `expected` byte for byte.  Both images
// hold opaque 32-bit pixels (PDF backgrounds fill white before drawing), so
// straight and premultiplied ARGB32 agree; the raw layouts do too.
bool ImageRegionMatches(const QImage& expected, const QRect& rect, const QImage& actual) {
    if (actual.isNull() || actual.width() != rect.width() || actual.height() != rect.height()
        || !expected.rect().contains(rect)) {
        return false;
    }
    for (int row = 0; row < rect.height(); ++row) {
        const uchar* expectedRow = expected.constScanLine(rect.y() + row) + (rect.x() * 4);
        const uchar* actualRow = actual.constScanLine(row);
        if (std::memcmp(expectedRow, actualRow, static_cast<std::size_t>(rect.width()) * 4U) != 0) {
            return false;
        }
    }
    return true;
}

cnote::Page RotatedPage(const cnote::Page& page, int rotationDegrees) {
    cnote::Page rotated = page;
    if ((rotationDegrees / 90) % 2 != 0) {
        std::swap(rotated.width, rotated.height);
    }
    rotated.rotationDegrees = static_cast<float>(rotationDegrees);
    return rotated;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    cnote::PdfiumBackend backend;
    const std::size_t loadsBeforeOpen = backend.DocumentLoadCount();
    const double openStart = NowMilliseconds();
    auto document = backend.OpenFromMemory(
        std::filesystem::path("multi-page.pdf"),
        std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(50)));
    const double openEnd = NowMilliseconds();
    Check(document.has_value() && document->PageCount() == 50U, "50-page PDF opens");
    Check(backend.DocumentLoadCount() == loadsBeforeOpen + 1U, "opening the PDF loads its document once");
    if (!document.has_value()) {
        return 1;
    }
    Check(document->PageAt(0).raster.pixels.empty(), "cold open does not materialize page rasters");

    cnote::render::PdfiumBackgroundProvider provider(backend, *document, 8);
    Check(provider.RenderCount() == 0U, "cold open renders no PDF pages");

    cnote::render::PageRenderParams params;
    params.scale = 1.0f;
    params.pdfBackground = &provider;

    const double firstRenderStart = NowMilliseconds();
    const QImage page0 = cnote::render::RenderPageSurface(document->PageAt(0), params);
    const double firstRenderEnd = NowMilliseconds();
    std::printf("TIMING load50=%.2fms firstRender=%.2fms\n",
        openEnd - openStart, firstRenderEnd - firstRenderStart);
    Check(provider.RenderCount() == 1U, "rendering one page renders exactly one PDF page");
    Check(provider.CachedPageCount() == 1U, "one page is cached");
    Check(page0.width() == 612 && page0.height() == 792, "page surface uses the page size");
    Check(HasNonWhitePixel(page0), "PDF page content reaches the rendered surface");

    const QImage page1 = cnote::render::RenderPageSurface(document->PageAt(1), params);
    Check(provider.RenderCount() == 2U, "rendering a second page renders one more PDF page");
    Check(HasNonWhitePixel(page1), "second page content reaches the rendered surface");

    static_cast<void>(cnote::render::RenderPageSurface(document->PageAt(0), params));
    Check(provider.RenderCount() == 2U, "re-rendering the same page and scale is served from cache");

    cnote::render::PageRenderParams zoomed = params;
    zoomed.scale = 2.0f;
    static_cast<void>(cnote::render::RenderPageSurface(document->PageAt(0), zoomed));
    Check(provider.RenderCount() == 3U, "zooming re-renders at the new raster scale");

    // --- every operation on one PdfSource shares one open document handle ---
    {
        const std::size_t loadsAfterOpen = backend.DocumentLoadCount();
        // The provider's renders above, a direct render, text extraction and
        // hydration must all reuse the handle the open created.
        const float parityScale = 3.0f;
        const auto direct = backend.RenderPage(document->PdfData(), 0U, parityScale);
        Check(direct.has_value(), "a direct PDF render succeeds");
        const QImage viaProvider = provider.PageBackground(0, 0, parityScale);
        Check(!viaProvider.isNull(), "the provider renders at the parity scale");
        if (direct.has_value() && !viaProvider.isNull()) {
            Check(viaProvider.width() == direct->width && viaProvider.height() == direct->height,
                "provider output has the direct raster's size");
            Check(viaProvider.bytesPerLine() == direct->stride
                    && viaProvider.format() == QImage::Format_ARGB32,
                "provider output keeps the direct raster's stride and format");
            bool identical = viaProvider.bytesPerLine() == direct->stride;
            for (int row = 0; identical && row < direct->height; ++row) {
                identical = std::memcmp(viaProvider.constScanLine(row),
                                direct->pixels.data() + (static_cast<std::size_t>(row) * static_cast<std::size_t>(direct->stride)),
                                static_cast<std::size_t>(direct->stride))
                    == 0;
            }
            Check(identical, "provider output is byte-identical to the direct PDF raster");
        }
        Check(backend.DocumentLoadCount() == loadsAfterOpen, "page renders reuse the open document");

        Check(backend.ExtractTextCharBoxes(document->PdfData(), 0U).has_value(),
            "PDF text boxes extract from page 0");
        Check(backend.DocumentLoadCount() == loadsAfterOpen,
            "text extraction reuses the open document instead of reloading it");

        backend.HydrateProject(*document);
        Check(backend.DocumentLoadCount() == loadsAfterOpen,
            "hydration reuses the open document instead of reloading it");
        Check(document->PageAt(0).sourcePdfSourceIndex == 0
                && document->PageAt(0).sourcePdfPageIndex == 0,
            "hydration keeps a referenced, readable source page");
    }

    // Bounded region renders: the canvas tiles above the raster budget with
    // these.  A region starting at the page origin must equal the full raster
    // exactly; a shifted origin draws the same content (PDFium re-rasterizes
    // glyph anti-aliasing for the new device position, so allow a small
    // anti-aliasing delta).  Every tile of a tiled page shares one region
    // render for the PDF background, so shifted renders never abut each other.
    {
        const auto pdfBytes = std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(1));
        const float scale = 1.75f;
        const auto full = backend.RenderPage(pdfBytes, 0U, scale);
        Check(full.has_value() && full->width == 1071 && full->height == 1386,
            "region probe: full PDF raster has the expected size");
        if (full.has_value()) {
            const auto wholeRegion = backend.RenderPageRegion(
                pdfBytes, 0U, scale, cnote::PdfPixelRect { 0, 0, full->width, full->height });
            Check(wholeRegion.has_value()
                    && std::memcmp(wholeRegion->pixels.data(), full->pixels.data(),
                           static_cast<std::size_t>(full->height) * static_cast<std::size_t>(full->stride)) == 0,
                "a region covering the whole page equals the full PDF raster");

            const QRect shiftRect(196, 0, full->width - 196, full->height);
            const auto shifted = backend.RenderPageRegion(pdfBytes, 0U, scale,
                cnote::PdfPixelRect { shiftRect.x(), shiftRect.y(), shiftRect.width(), shiftRect.height() });
            Check(shifted.has_value(), "a shifted PDF region renders");
            if (shifted.has_value()) {
                int differingPixels = 0;
                int maxChannelDelta = 0;
                for (int row = 0; row < shiftRect.height(); ++row) {
                    const std::uint8_t* expected = full->pixels.data()
                        + (static_cast<std::size_t>(row) * static_cast<std::size_t>(full->stride))
                        + (static_cast<std::size_t>(shiftRect.x()) * 4U);
                    const std::uint8_t* actual = shifted->pixels.data()
                        + (static_cast<std::size_t>(row) * static_cast<std::size_t>(shifted->stride));
                    for (int column = 0; column < shiftRect.width(); ++column) {
                        bool differs = false;
                        for (int channel = 0; channel < 4; ++channel) {
                            const int delta = std::abs(static_cast<int>(expected[(column * 4) + channel])
                                - static_cast<int>(actual[(column * 4) + channel]));
                            maxChannelDelta = std::max(maxChannelDelta, delta);
                            differs = differs || delta != 0;
                        }
                        differingPixels += differs ? 1 : 0;
                    }
                }
                const int totalPixels = shiftRect.width() * shiftRect.height();
                std::printf("PDF-REGION shifted differing=%d/%d maxChannelDelta=%d\n",
                    differingPixels, totalPixels, maxChannelDelta);
                Check(differingPixels * 20 < totalPixels,
                    "a shifted PDF region differs only in glyph anti-aliasing");
                Check(maxChannelDelta <= 96, "shifted PDF region anti-aliasing delta stays small");
            }
        }

        // The provider caches region rasters, serves them from that cache, and
        // charges them to its byte budget.
        const QImage fullBackground = provider.PageBackground(0, 0, scale);
        Check(!fullBackground.isNull(), "region probe: full background renders through the provider");
        const QRect regionRect(0, 0, 196, 140);
        const std::optional<QImage> region = provider.PageBackgroundRegion(0, 0, scale, regionRect);
        Check(region.has_value(), "region probe: the provider serves a bounded region");
        if (region.has_value()) {
            Check(region->size() == regionRect.size(), "provider region has the requested size");
            const std::size_t rendersBefore = provider.RenderCount();
            const std::optional<QImage> again = provider.PageBackgroundRegion(0, 0, scale, regionRect);
            Check(again.has_value() && provider.RenderCount() == rendersBefore,
                "a repeated region request is served from the provider cache");
        }
        Check(provider.CachedBytes() <= cnote::render::kPdfBackgroundCacheBytes,
            "provider cache bytes stay within the budget");
    }

    // --- small patches crop the page background instead of re-rendering ----
    // A warm full-page raster is the only pixel source an erase or move patch
    // may use: a fresh PDFium excerpt with a shifted device origin re-
    // rasterizes glyph anti-aliasing.  All of these patch rects therefore have
    // to be exact crops, with no additional PDFium render of any kind.
    {
        const auto patchBytes = std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(1));
        cnote::Page patchPage;
        patchPage.width = 612.0f;
        patchPage.height = 792.0f;
        patchPage.sourcePdfSourceIndex = 0;
        patchPage.sourcePdfPageIndex = 0;
        cnote::Document patchDocument = cnote::Document::CreateProjectDocument(
            std::filesystem::path("patch.cnote"),
            { { std::filesystem::path("patch.pdf"), patchBytes } },
            { patchPage },
            0,
            {});
        cnote::render::PdfiumBackgroundProvider patchProvider(backend, patchDocument, 8);

        const float patchScale = 1.75f;
        const QImage warmFull = patchProvider.PageBackground(0, 0, patchScale);
        Check(!warmFull.isNull(), "patch: the full-page background is warm");
        Check(patchProvider.FullPageRenderCount() == 1U, "patch: warming renders one full page");
        Check(patchProvider.RegionRenderCount() == 0U, "patch: warming renders no region");

        // Text sits in the upper left of this generated page, so the first
        // patch covers glyph anti-aliasing; the others cover blank paper and
        // the far edge.
        const QRect patchRects[] = {
            QRect(100, 60, 240, 90),
            QRect(10, 10, 40, 24),
            QRect(warmFull.width() - 90, warmFull.height() - 70, 90, 70),
            QRect(300, 400, 64, 48),
        };
        for (const QRect& patchRect : patchRects) {
            const std::optional<QImage> patch = patchProvider.PageBackgroundRegion(
                0, 0, patchScale, patchRect, warmFull.size());
            Check(patch.has_value() && patch->size() == patchRect.size(),
                "patch: a warm page serves a patch of the requested size");
            if (patch.has_value()) {
                Check(ImageRegionMatches(warmFull, patchRect, *patch),
                    "patch: warm patch pixels are an exact crop of the page raster");
            }
        }
        Check(patchProvider.FullPageRenderCount() == 1U,
            "patch: small patches trigger no additional full-page render");
        Check(patchProvider.RegionRenderCount() == 0U,
            "patch: small patches trigger no region render when the full raster is warm");
    }

    // Cold: the first patch renders the full page once into the cache and
    // crops it, so the surface that follows reuses exactly those pixels.
    {
        const auto coldBytes = std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(1));
        cnote::Page coldPage;
        coldPage.width = 612.0f;
        coldPage.height = 792.0f;
        coldPage.sourcePdfSourceIndex = 0;
        coldPage.sourcePdfPageIndex = 0;
        cnote::Document coldDocument = cnote::Document::CreateProjectDocument(
            std::filesystem::path("cold-patch.cnote"),
            { { std::filesystem::path("cold-patch.pdf"), coldBytes } },
            { coldPage },
            0,
            {});
        cnote::render::PdfiumBackgroundProvider coldProvider(backend, coldDocument, 8);
        const QSize fullSize(612, 792);
        const QRect firstRect(100, 60, 240, 90);
        const std::optional<QImage> coldPatch = coldProvider.PageBackgroundRegion(
            0, 0, 1.0f, firstRect, fullSize);
        Check(coldPatch.has_value() && coldPatch->size() == firstRect.size(),
            "cold patch: the first patch is served");
        Check(coldProvider.FullPageRenderCount() == 1U,
            "cold patch: the first patch renders the full page exactly once");
        Check(coldProvider.RegionRenderCount() == 0U,
            "cold patch: the first patch does not render a separate region");

        const QImage coldFull = coldProvider.PageBackground(0, 0, 1.0f);
        Check(!coldFull.isNull(), "cold patch: the page surface reuses the cached raster");
        Check(coldProvider.FullPageRenderCount() == 1U,
            "cold patch: the following page surface triggers no further full-page render");
        if (coldPatch.has_value() && !coldFull.isNull()) {
            Check(ImageRegionMatches(coldFull, firstRect, *coldPatch),
                "cold patch: the patch is an exact crop of the page the surface uses");
        }

        const QRect secondRect(300, 400, 64, 48);
        const std::optional<QImage> secondPatch = coldProvider.PageBackgroundRegion(
            0, 0, 1.0f, secondRect, fullSize);
        Check(secondPatch.has_value() && secondPatch->size() == secondRect.size(),
            "cold patch: later patches are served");
        Check(coldProvider.FullPageRenderCount() == 1U && coldProvider.RegionRenderCount() == 0U,
            "cold patch: later patches use the warm full raster");

        // Above the raster budget there is no full-page raster to crop: the
        // provider renders just the requested rect (documented glyph
        // anti-aliasing allowance for tile-sized pages).
        cnote::render::PdfiumBackgroundProvider oversizedProvider(backend, coldDocument, 8);
        const QRect boundedRect(200, 200, 64, 48);
        const std::optional<QImage> bounded = oversizedProvider.PageBackgroundRegion(
            0, 0, 1.0f, boundedRect, QSize(cnote::render::kMaxRasterDimension + 1, 900));
        Check(bounded.has_value() && bounded->size() == boundedRect.size(),
            "over-budget patch: only the requested region renders");
        Check(oversizedProvider.FullPageRenderCount() == 0U
                && oversizedProvider.RegionRenderCount() == 1U,
            "over-budget patch: no full-page raster is allocated for one patch");
    }

    // --- region renders are pixel-identical to the full-page surface -------
    // This is the end-to-end parity an erase or move patch needs: the region
    // renderer (deviceRegion + pdfBackgroundRegion plumbing) must reproduce the
    // full surface's background pixels even though it paints only a slice, at
    // quarter turns about the page centre and at non-1 device pixel ratios.
    {
        const auto parityBytes = std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(1, true));
        cnote::Page parityPage;
        parityPage.width = 612.0f;
        parityPage.height = 792.0f;
        parityPage.sourcePdfSourceIndex = 0;
        parityPage.sourcePdfPageIndex = 0;
        cnote::Document parityDocument = cnote::Document::CreateProjectDocument(
            std::filesystem::path("parity.cnote"),
            { { std::filesystem::path("parity.pdf"), parityBytes } },
            { parityPage },
            0,
            {});

        const struct ParityCase {
            float scale;
            float devicePixelRatio;
            int rotationDegrees;
        } parityCases[] = {
            { 1.0f, 1.0f, 0 },
            { 1.75f, 1.0f, 0 },
            { 1.0f, 2.0f, 0 },
            { 1.25f, 1.0f, 90 },
            { 1.0f, 1.0f, 180 },
            { 1.5f, 1.0f, 270 },
        };
        for (const ParityCase& parityCase : parityCases) {
            const cnote::Page page = RotatedPage(parityPage, parityCase.rotationDegrees);
            cnote::render::PdfiumBackgroundProvider parityProvider(backend, parityDocument, 8);
            cnote::render::PageRenderParams parityParams;
            parityParams.scale = parityCase.scale;
            parityParams.devicePixelRatio = parityCase.devicePixelRatio;
            parityParams.pdfBackground = &parityProvider;

            const QImage full = cnote::render::RenderPageSurface(page, parityParams);
            Check(!full.isNull(), "parity: the full surface renders");
            if (full.isNull()) {
                continue;
            }
            Check(HasNonWhitePixel(full), "parity: the fixture has content to compare");
            Check(parityProvider.FullPageRenderCount() == 1U,
                "parity: the full surface renders the PDF page once");

            const int width = full.width();
            const int height = full.height();
            // The four quadrants always include the generated page's one text
            // run, whatever the quarter turn, so glyph anti-aliasing is part
            // of the comparison.
            const QRect regions[] = {
                QRect(0, 0, width / 2, height / 2),
                QRect(width / 2, 0, width - (width / 2), height / 2),
                QRect(0, height / 2, width / 2, height - (height / 2)),
                QRect(width / 2, height / 2, width - (width / 2), height - (height / 2)),
                QRect(std::max(0, (width / 2) - 50), std::max(0, (height / 2) - 40),
                    std::min(100, width), std::min(80, height)),
            };
            for (const QRect& region : regions) {
                if (region.isEmpty()) {
                    continue;
                }
                const QImage patch = cnote::render::RenderPageSurfaceRegionPixels(
                    page, parityParams, region);
                Check(!patch.isNull() && patch.size() == region.size(),
                    "parity: the region render produces the requested raster");
                if (!patch.isNull()) {
                    Check(ImageRegionMatches(full, region, patch),
                        "parity: region render pixels equal the full surface");
                }
            }

            // Tiled pages share one background render of the visible page
            // rect across every tile (pdfBackgroundRegion).  Each tile must
            // still equal the same pixels of the full surface, including the
            // tiles whose origin is far from the page origin.
            {
                const QRect visibleRegion(width / 4, height / 4,
                    std::max(1, width / 2), std::max(1, height / 2));
                cnote::render::PageRenderParams tileParams = parityParams;
                tileParams.pdfBackgroundRegion = visibleRegion;
                const int midX = visibleRegion.x() + (visibleRegion.width() / 2);
                const int midY = visibleRegion.y() + (visibleRegion.height() / 2);
                const QRect tiles[] = {
                    QRect(visibleRegion.x(), visibleRegion.y(),
                        std::max(1, visibleRegion.width() / 3),
                        std::max(1, visibleRegion.height() / 3)),
                    QRect(midX, midY,
                        std::max(1, visibleRegion.right() + 1 - midX),
                        std::max(1, visibleRegion.bottom() + 1 - midY)),
                };
                for (const QRect& tile : tiles) {
                    const QRect clamped = tile.intersected(QRect(0, 0, width, height));
                    if (clamped.isEmpty()) {
                        continue;
                    }
                    const QImage image = cnote::render::RenderPageSurfaceRegionPixels(
                        page, tileParams, clamped);
                    Check(!image.isNull(), "parity: a tiled-page render produces a raster");
                    if (!image.isNull()) {
                        Check(ImageRegionMatches(full, clamped, image),
                            "parity: a tile matches the full surface through the shared background");
                    }
                }
            }
            std::printf("PARITY rot=%d scale=%.2f dpr=%.2f fullRenders=%zu regionRenders=%zu\n",
                parityCase.rotationDegrees, static_cast<double>(parityCase.scale),
                static_cast<double>(parityCase.devicePixelRatio),
                parityProvider.FullPageRenderCount(), parityProvider.RegionRenderCount());
            Check(parityProvider.FullPageRenderCount() == 1U,
                "parity: region renders trigger no additional full-page render");
            Check(parityProvider.RegionRenderCount() == 0U,
                "parity: region renders crop the warm full-page raster");
        }

        // Above the full-page raster budget (a tiled page) there is no full
        // raster to crop: the visible page rect is rendered once as a bounded
        // region, and later patches inside it crop that render instead of
        // asking PDFium for another excerpt.
        {
            cnote::render::PdfiumBackgroundProvider tiledProvider(backend, parityDocument, 8);
            const float tiledScale = 4.0f;
            const QSize tiledPageSize(612 * 4, 792 * 4);
            Check(!cnote::render::RasterFitsBudget(tiledPageSize.width(), tiledPageSize.height()),
                "tiled patch: the probe page is over the full-raster budget");
            const QRect visibleRegion(400, 600, 900, 700);
            const std::optional<QImage> visible = tiledProvider.PageBackgroundRegion(
                0, 0, tiledScale, visibleRegion, tiledPageSize);
            Check(visible.has_value() && visible->size() == visibleRegion.size(),
                "tiled patch: the visible background region renders");
            Check(tiledProvider.FullPageRenderCount() == 0U
                    && tiledProvider.RegionRenderCount() == 1U,
                "tiled patch: the first region request renders only that region");
            const QRect patchRect(500, 700, 120, 90);
            const std::optional<QImage> cropped = tiledProvider.PageBackgroundRegion(
                0, 0, tiledScale, patchRect, tiledPageSize);
            Check(cropped.has_value() && cropped->size() == patchRect.size(),
                "tiled patch: a patch inside the visible region is served");
            Check(tiledProvider.FullPageRenderCount() == 0U
                    && tiledProvider.RegionRenderCount() == 1U,
                "tiled patch: a patch inside the warm visible region renders nothing");
            if (visible.has_value() && cropped.has_value()) {
                Check(ImageRegionMatches(*visible, patchRect.translated(-visibleRegion.topLeft()),
                        *cropped),
                    "tiled patch: the patch is an exact crop of the visible region");
            }
        }

        // Cold single patch followed by the full surface: the surface reuses
        // the one full-page render instead of re-rasterizing the page.
        {
            const cnote::Page page = RotatedPage(parityPage, 0);
            cnote::render::PdfiumBackgroundProvider coldParityProvider(backend, parityDocument, 8);
            cnote::render::PageRenderParams coldParams;
            coldParams.scale = 1.75f;
            coldParams.pdfBackground = &coldParityProvider;
            const QRect coldRegion(100, 60, 240, 90);
            const QImage coldPatch = cnote::render::RenderPageSurfaceRegionPixels(
                page, coldParams, coldRegion);
            Check(!coldPatch.isNull(), "cold parity: a cold patch renders");
            Check(coldParityProvider.FullPageRenderCount() == 1U
                    && coldParityProvider.RegionRenderCount() == 0U,
                "cold parity: a cold patch renders the full page once");
            const QImage coldFull = cnote::render::RenderPageSurface(page, coldParams);
            Check(!coldFull.isNull(), "cold parity: the full surface renders");
            if (!coldPatch.isNull() && !coldFull.isNull()) {
                Check(ImageRegionMatches(coldFull, coldRegion, coldPatch),
                    "cold parity: the patch matches the surface's own pixels");
            }
            Check(coldParityProvider.FullPageRenderCount() == 1U,
                "cold parity: the surface adds no full-page render");
        }
    }

    // Pan across all 50 pages with a fresh provider: each page renders once and
    // the cache stays bounded.
    cnote::render::PdfiumBackgroundProvider panProvider(backend, *document, 8);
    cnote::render::PageRenderParams panParams = params;
    panParams.pdfBackground = &panProvider;
    for (std::size_t pageIndex = 0; pageIndex < document->PageCount(); ++pageIndex) {
        static_cast<void>(cnote::render::RenderPageSurface(document->PageAt(pageIndex), panParams));
    }
    Check(panProvider.RenderCount() == 50U, "panning renders each page once");
    Check(panProvider.CachedPageCount() <= 8U, "cache stays bounded while panning a 50-page PDF");

    // Thumbnail and canvas renders have separate budgets: scrolling the
    // sidebar must not evict the canvas page the user is looking at, and a
    // canvas pan must not evict every thumbnail.
    {
        cnote::render::PdfiumBackgroundProvider splitProvider(backend, *document, 2);
        cnote::render::PageRenderParams splitParams = params;
        splitParams.pdfBackground = &splitProvider;
        static_cast<void>(cnote::render::RenderPageSurface(document->PageAt(0), splitParams));
        static_cast<void>(cnote::render::RenderPageSurface(document->PageAt(1), splitParams));
        Check(splitProvider.RenderCount() == 2U, "split budget: two canvas pages render");
        for (int page = 0; page < 12; ++page) {
            Check(!splitProvider.PageBackground(0, page, 0.2f, cnote::render::PdfBackgroundUse::Thumbnail).isNull(),
                "split budget: a thumbnail renders");
        }
        Check(splitProvider.RenderCount() == 14U, "split budget: each thumbnail rendered once");
        const std::size_t beforeCanvasHit = splitProvider.RenderCount();
        static_cast<void>(cnote::render::RenderPageSurface(document->PageAt(0), splitParams));
        static_cast<void>(cnote::render::RenderPageSurface(document->PageAt(1), splitParams));
        Check(splitProvider.RenderCount() == beforeCanvasHit,
            "thumbnail churn does not evict canvas entries from their own budget");
        const std::size_t beforeThumbnailHit = splitProvider.RenderCount();
        Check(!splitProvider.PageBackground(0, 0, 0.2f, cnote::render::PdfBackgroundUse::Thumbnail).isNull(),
            "split budget: an early thumbnail is still cached");
        Check(splitProvider.RenderCount() == beforeThumbnailHit,
            "canvas entries do not evict thumbnails from their own budget");
        Check(splitProvider.CachedBytes(cnote::render::PdfBackgroundUse::Canvas)
                <= cnote::render::kPdfBackgroundCacheBytes,
            "canvas background bytes stay within the canvas budget");
        Check(splitProvider.CachedBytes(cnote::render::PdfBackgroundUse::Thumbnail)
                <= cnote::render::kPdfThumbnailCacheBytes,
            "thumbnail background bytes stay within the thumbnail budget");
    }

    // --- a failed provider render is retried after the source identity changes
    {
        auto retrySource = std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(1));
        cnote::Page retryPage0;
        retryPage0.width = 612.0f;
        retryPage0.height = 792.0f;
        retryPage0.sourcePdfSourceIndex = 0;
        retryPage0.sourcePdfPageIndex = 0;
        cnote::Page retryPage1 = retryPage0;
        retryPage1.sourcePdfPageIndex = 1;  // beyond the one-page source
        cnote::Document retryDocument = cnote::Document::CreateProjectDocument(
            std::filesystem::path("retry.cnote"),
            { { std::filesystem::path("one-page.pdf"), retrySource } },
            { retryPage0, retryPage1 },
            0,
            {});
        cnote::render::PdfiumBackgroundProvider retryProvider(backend, retryDocument, 8);

        Check(retryProvider.PageBackground(0, 1, 1.0f).isNull(),
            "a failed source page render yields no background");
        Check(retryProvider.FailedRenderCount() == 1U, "the failed render was attempted once");
        static_cast<void>(retryProvider.PageBackground(0, 1, 1.0f));
        Check(retryProvider.FailedRenderCount() == 1U, "the failed render is negatively cached");
        Check(!retryProvider.PageBackground(0, 0, 1.0f).isNull(), "a valid source page renders");
        Check(retryProvider.RenderCount() == 1U, "the valid source page was rendered once");

        // Same source index and page, different PDF bytes: the failure must be
        // retried instead of being served from the negative cache.
        auto widerSource = std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(2));
        std::vector<cnote::Page> retryPages { retryDocument.PageAt(0), retryDocument.PageAt(1) };
        retryDocument.ReplaceWithProject(
            std::filesystem::path("retry.cnote"),
            { { std::filesystem::path("two-page.pdf"), widerSource } },
            std::move(retryPages),
            0,
            {});
        Check(!retryProvider.PageBackground(0, 1, 1.0f).isNull(),
            "a failed render is retried after the source identity changes");
        Check(retryProvider.RenderCount() == 2U, "the retry rendered the background");
        Check(retryProvider.FailedRenderCount() == 1U, "the retry was not another failed attempt");

        // Source validation happens before a cache lookup: removing the source
        // must not serve the previously cached background.
        retryDocument.ReplaceWithProject(
            std::filesystem::path("retry.cnote"),
            {},
            { retryDocument.PageAt(0) },
            0,
            {});
        Check(retryProvider.PageBackground(0, 0, 1.0f).isNull(),
            "a removed source is validated before a cache hit is served");
        retryProvider.Invalidate();
        Check(retryProvider.CachedPageCount() == 0U,
            "Invalidate drops positive and negative entries");
    }

    // --- the cached handle owns the source bytes until it closes ------------
    // PDFium parses from the caller's buffer, so the cache must keep the bytes
    // alive until after FPDF_CloseDocument. Dropping the document and releasing
    // it closes the handle first and only then frees the buffer.
    {
        // Settle entries left behind by earlier blocks so the counters below
        // measure this block only.
        cnote::PdfiumBackend::ReleaseUnreferencedDocuments();
        auto ownedBytes = std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(1));
        std::weak_ptr<std::vector<std::uint8_t>> ownedBytesAlive = ownedBytes;
        const std::size_t loadsBefore = backend.DocumentLoadCount();
        const std::size_t closesBefore = backend.DocumentCloseCount();
        const std::size_t cachedBefore = backend.CachedDocumentCount();
        {
            auto ownedDocument = backend.OpenFromMemory(
                std::filesystem::path("owned.pdf"), ownedBytes);
            Check(ownedDocument.has_value(), "a PDF whose bytes the test owns opens");
            Check(backend.DocumentLoadCount() == loadsBefore + 1U,
                "opening that PDF loads its document once");
            Check(backend.CachedDocumentCount() == cachedBefore + 1U,
                "the open handle is cached");
            if (ownedDocument.has_value()) {
                cnote::render::PdfiumBackgroundProvider ownedProvider(backend, *ownedDocument, 8);
                Check(!ownedProvider.PageBackground(0, 0, 1.0f).isNull(),
                    "the cached handle renders the page");
                Check(backend.DocumentLoadCount() == loadsBefore + 1U,
                    "rendering through the cache does not reload");
            }
            ownedBytes.reset();
            ownedDocument.reset();
        }
        Check(!ownedBytesAlive.expired(),
            "the cache keeps the source bytes alive while its handle is open");
        Check(backend.DocumentCloseCount() == closesBefore,
            "the handle is still open before the release hook");
        Check(backend.CachedDocumentCount() == cachedBefore + 1U,
            "the unreleased handle is still cached");
        cnote::PdfiumBackend::ReleaseUnreferencedDocuments();
        Check(backend.DocumentCloseCount() == closesBefore + 1U,
            "the release hook closes the unreferenced handle");
        Check(ownedBytesAlive.expired(),
            "the source bytes are freed only after the handle closed");
        Check(backend.CachedDocumentCount() == cachedBefore,
            "the release hook drops the cached entry");
    }

    // Replacing a document's source drops the old handle on the next acquire,
    // closing it before the old bytes are released.
    {
        auto replacedBytes = std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(1));
        std::weak_ptr<std::vector<std::uint8_t>> replacedBytesAlive = replacedBytes;
        cnote::Page replacedPage;
        replacedPage.width = 612.0f;
        replacedPage.height = 792.0f;
        replacedPage.sourcePdfSourceIndex = 0;
        replacedPage.sourcePdfPageIndex = 0;
        cnote::Document replacedDocument = cnote::Document::CreateProjectDocument(
            std::filesystem::path("replaced.cnote"),
            { { std::filesystem::path("replaced.pdf"), replacedBytes } },
            { replacedPage },
            0,
            {});
        Check(backend.RenderPage(replacedDocument.PdfData(), 0U, 1.0f).has_value(),
            "the replaced document's source renders");
        const std::size_t closesBefore = backend.DocumentCloseCount();
        replacedDocument.ReplaceWithProject(
            std::filesystem::path("replaced.cnote"),
            { { std::filesystem::path("replacement.pdf"),
                std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(1)) } },
            { replacedDocument.PageAt(0) },
            0,
            {});
        replacedBytes.reset();
        Check(!replacedBytesAlive.expired(),
            "the replaced source is still pinned by its cached handle");
        Check(backend.RenderPage(replacedDocument.PdfData(), 0U, 1.0f).has_value(),
            "the replacement source renders");
        Check(replacedBytesAlive.expired(),
            "the next acquire closed the replaced handle before freeing its bytes");
        Check(backend.DocumentCloseCount() > closesBefore,
            "the replaced document's handle was closed");
    }

    // --- hydration never leaves a page claiming a source it cannot show -----
    {
        const cnote::RasterImage storedRaster =
            cnote::CreateSolidRaster(120.0f, 160.0f, 1.0f, 0xFF336699U);

        cnote::Page missingSourcePage;
        missingSourcePage.width = 120.0f;
        missingSourcePage.height = 160.0f;
        missingSourcePage.sourcePdfSourceIndex = 0;
        missingSourcePage.sourcePdfPageIndex = 0;
        missingSourcePage.raster = storedRaster;
        cnote::Document missingDocument = cnote::Document::CreateProjectDocument(
            std::filesystem::path("missing.cnote"),
            { { std::filesystem::path("missing.pdf"), std::make_shared<std::vector<std::uint8_t>>() } },
            { missingSourcePage },
            0,
            {});
        backend.HydrateProject(missingDocument);
        Check(missingDocument.PageAt(0).sourcePdfSourceIndex == -1
                && missingDocument.PageAt(0).sourcePdfPageIndex == -1,
            "hydration clears source indices for a missing source");
        Check(cnote::RasterImageEqual(missingDocument.PageAt(0).raster, storedRaster),
            "hydration keeps the stored raster for a missing source");

        cnote::Page corruptSourcePage;
        corruptSourcePage.width = 120.0f;
        corruptSourcePage.height = 160.0f;
        corruptSourcePage.sourcePdfSourceIndex = 0;
        corruptSourcePage.sourcePdfPageIndex = 0;
        corruptSourcePage.raster = storedRaster;
        cnote::Document corruptDocument = cnote::Document::CreateProjectDocument(
            std::filesystem::path("corrupt.cnote"),
            { { std::filesystem::path("corrupt.pdf"),
                std::make_shared<std::vector<std::uint8_t>>(8, 0x7FU) } },
            { corruptSourcePage },
            0,
            {});
        backend.HydrateProject(corruptDocument);
        Check(corruptDocument.PageAt(0).sourcePdfSourceIndex == -1
                && corruptDocument.PageAt(0).sourcePdfPageIndex == -1,
            "hydration clears source indices for an invalid source");
        Check(cnote::RasterImageEqual(corruptDocument.PageAt(0).raster, storedRaster),
            "hydration keeps the stored raster for an invalid source");

        cnote::Page badPageIndex;
        badPageIndex.width = 612.0f;
        badPageIndex.height = 792.0f;
        badPageIndex.sourcePdfSourceIndex = 0;
        badPageIndex.sourcePdfPageIndex = 3;  // the source has one page
        badPageIndex.raster = storedRaster;
        cnote::Document badPageIndexDocument = cnote::Document::CreateProjectDocument(
            std::filesystem::path("bad-page.cnote"),
            { { std::filesystem::path("one-page.pdf"),
                std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(1)) } },
            { badPageIndex },
            0,
            {});
        backend.HydrateProject(badPageIndexDocument);
        Check(badPageIndexDocument.PageAt(0).sourcePdfSourceIndex == -1
                && badPageIndexDocument.PageAt(0).sourcePdfPageIndex == -1,
            "hydration clears source indices when the source page cannot render");
        Check(cnote::RasterImageEqual(badPageIndexDocument.PageAt(0).raster, storedRaster),
            "hydration keeps the stored raster when the source page cannot render");

        cnote::Page lazyPage;
        lazyPage.width = 612.0f;
        lazyPage.height = 792.0f;
        lazyPage.sourcePdfSourceIndex = 0;
        lazyPage.sourcePdfPageIndex = 0;
        cnote::Document lazyDocument = cnote::Document::CreateProjectDocument(
            std::filesystem::path("lazy.cnote"),
            { { std::filesystem::path("one-page.pdf"),
                std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(1)) } },
            { lazyPage },
            0,
            {});
        backend.HydrateProject(lazyDocument);
        Check(lazyDocument.PageAt(0).sourcePdfSourceIndex == 0
                && lazyDocument.PageAt(0).sourcePdfPageIndex == 0,
            "hydration keeps valid source indices");
        Check(lazyDocument.PageAt(0).raster.pixels.empty(),
            "hydration leaves a valid source background lazy");

        // A page whose referenced source page cannot be read must keep its
        // size, rotation and slot, claim no source, and represent the blank
        // background without allocating a full fallback raster.
        cnote::Page unreadablePage;
        unreadablePage.width = 300.0f;
        unreadablePage.height = 400.0f;
        unreadablePage.rotationDegrees = 90.0f;
        unreadablePage.sourcePdfSourceIndex = 0;
        unreadablePage.sourcePdfPageIndex = 7;  // the source has one page
        cnote::Page readableNeighbour;
        readableNeighbour.width = 612.0f;
        readableNeighbour.height = 792.0f;
        readableNeighbour.sourcePdfSourceIndex = 0;
        readableNeighbour.sourcePdfPageIndex = 0;
        cnote::Document unreadableDocument = cnote::Document::CreateProjectDocument(
            std::filesystem::path("unreadable.cnote"),
            { { std::filesystem::path("one-page.pdf"),
                std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(1)) } },
            { unreadablePage, readableNeighbour },
            0,
            {});
        backend.HydrateProject(unreadableDocument);
        Check(unreadableDocument.PageCount() == 2U,
            "hydration keeps the page list, so page indices do not shift");
        const cnote::Page& unreadable = unreadableDocument.PageAt(0);
        Check(unreadable.sourcePdfSourceIndex == -1 && unreadable.sourcePdfPageIndex == -1,
            "an unreadable source page claims no source");
        Check(unreadable.raster.pixels.empty(),
            "an unreadable source page allocates no full fallback raster");
        Check(unreadable.width == 300.0f && unreadable.height == 400.0f
                && unreadable.rotationDegrees == 90.0f,
            "an unreadable source page keeps its size and rotation");
        Check(unreadableDocument.PageAt(1).sourcePdfSourceIndex == 0
                && unreadableDocument.PageAt(1).sourcePdfPageIndex == 0,
            "a readable referenced neighbour keeps its source page");
        Check(unreadableDocument.PageAt(1).raster.pixels.empty(),
            "a readable neighbour's background stays lazy");

        cnote::render::PdfiumBackgroundProvider unreadableProvider(backend, unreadableDocument, 8);
        cnote::render::PageRenderParams unreadableParams;
        unreadableParams.scale = 1.0f;
        unreadableParams.pdfBackground = &unreadableProvider;
        const QImage blankSurface = cnote::render::RenderPageSurface(
            unreadableDocument.PageAt(0), unreadableParams);
        Check(blankSurface.width() == 300 && blankSurface.height() == 400,
            "an unreadable page renders at its own size");
        Check(!HasNonWhitePixel(blankSurface), "an unreadable page renders as blank paper");

        cnote::Page storedRasterPage = lazyPage;
        const cnote::RasterImage validStoredRaster =
            cnote::CreateSolidRaster(612.0f, 792.0f, 1.0f, 0xFF336699U);
        storedRasterPage.raster = validStoredRaster;
        cnote::Document storedRasterDocument = cnote::Document::CreateProjectDocument(
            std::filesystem::path("stored-raster.cnote"),
            { { std::filesystem::path("one-page.pdf"),
                std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(1)) } },
            { storedRasterPage },
            0,
            {});
        backend.HydrateProject(storedRasterDocument);
        Check(storedRasterDocument.PageAt(0).sourcePdfSourceIndex == 0
                && storedRasterDocument.PageAt(0).sourcePdfPageIndex == 0,
            "hydration keeps valid source indices with a stored raster");
        Check(cnote::RasterImageEqual(storedRasterDocument.PageAt(0).raster, validStoredRaster),
            "hydration does not overwrite a stored raster with the lazy source's empty one");
    }

    std::printf("renderCount=%zu cachedPages=%zu\n", provider.RenderCount(), provider.CachedPageCount());
    std::printf(g_failures == 0 ? "ALL PDF BACKGROUND CHECKS PASSED\n" : "%d PDF BACKGROUND CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
