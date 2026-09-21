// Self-contained lazy PDF-background provider test (migration plan step 3.4).
//
// Proves the Qt canvas gets PDF page backgrounds lazily: a cold open renders no
// pages, only requested pages are rendered, results are cached per raster scale
// (zoom re-renders), and the cache stays bounded while panning a 50-page PDF.

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <QGuiApplication>
#include <QImage>

#include "Document.h"
#include "PdfBackend.h"
#include "render/PageRenderer.h"
#include "render/PdfiumBackgroundProvider.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

std::vector<std::uint8_t> BuildMultiPagePdf(int pageCount) {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    std::string kids;
    for (int index = 0; index < pageCount; ++index) {
        kids += std::to_string(4 + (index * 2)) + " 0 R ";
    }
    objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageCount) + " >>");
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    for (int index = 0; index < pageCount; ++index) {
        const std::string content = "BT /F1 24 Tf 72 720 Td (Page " + std::to_string(index + 1) + ") Tj ET\n";
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

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    cnote::PdfiumBackend backend;
    auto document = backend.OpenFromMemory(
        std::filesystem::path("multi-page.pdf"),
        std::make_shared<std::vector<std::uint8_t>>(BuildMultiPagePdf(50)));
    Check(document.has_value() && document->PageCount() == 50U, "50-page PDF opens");
    if (!document.has_value()) {
        return 1;
    }
    Check(document->PageAt(0).raster.pixels.empty(), "cold open does not materialize page rasters");

    cnote::render::PdfiumBackgroundProvider provider(backend, *document, 8);
    Check(provider.RenderCount() == 0U, "cold open renders no PDF pages");

    cnote::render::PageRenderParams params;
    params.scale = 1.0f;
    params.pdfBackground = &provider;

    const QImage page0 = cnote::render::RenderPageSurface(document->PageAt(0), params);
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

    std::printf("renderCount=%zu cachedPages=%zu\n", provider.RenderCount(), provider.CachedPageCount());
    std::printf(g_failures == 0 ? "ALL PDF BACKGROUND CHECKS PASSED\n" : "%d PDF BACKGROUND CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
