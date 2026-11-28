// Self-contained Qt PDF-export contract test (migration plan step 3.4).
//
// Builds synthetic documents, exports embedded and attachment-free PDFs through
// the real service path, reopens them, and defends the export invariants. Needs
// no external fixtures; the selectable-text case embeds a minimal PDF.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include "Document.h"
#include "PdfBackend.h"
#include "PdfOverlay.h"
#include "ProjectFile.h"
#include "document/FileIO.h"
#include "latex/LatexEngine.h"
#include "render/PageRenderer.h"
#include "text/TextLayoutQt.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

// Minimal PDFium-parseable one-page PDF with a single text object.
std::vector<std::uint8_t> BuildTextPdf() {
    const std::string content = "BT /F1 24 Tf 72 720 Td (Hello PDF) Tj ET\n";
    const std::vector<std::string> objects {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };

    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets(1, 0);
    for (std::size_t index = 0; index < objects.size(); ++index) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(index + 1U) + " 0 obj\n" + objects[index] + "\nendobj\n";
    }

    const std::size_t xrefOffset = pdf.size();
    pdf += "xref\n0 6\n0000000000 65535 f \n";
    for (std::size_t index = 1; index < offsets.size(); ++index) {
        std::string offset = std::to_string(offsets[index]);
        pdf += std::string(10U - offset.size(), '0') + offset + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n";
    pdf += std::to_string(xrefOffset) + "\n%%EOF\n";
    return std::vector<std::uint8_t>(pdf.begin(), pdf.end());
}

void AppendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7FU) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else if (codePoint <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
}

std::string ExtractText(cnote::PdfiumBackend& backend, const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes, std::size_t pageIndex) {
    const auto boxes = backend.ExtractTextCharBoxes(pdfBytes, pageIndex);
    if (!boxes.has_value()) {
        return {};
    }
    std::string text;
    for (const cnote::PdfTextCharBox& box : *boxes) {
        AppendUtf8(text, box.unicode);
    }
    return text;
}

cnote::Document BuildSyntheticDocument() {
    cnote::Document document = cnote::Document::CreateBlank();
    cnote::Page& page = document.CurrentPage();
    page.width = 200.0f;
    page.height = 200.0f;
    page.raster.width = 200;
    page.raster.height = 200;
    page.raster.stride = 200 * 4;
    page.raster.scale = 1.0f;
    std::vector<std::uint8_t> background(static_cast<std::size_t>(page.raster.stride) * 200U, 0);
    for (std::size_t index = 0; index < background.size(); index += 4U) {
        background[index + 0U] = 0xCCU;  // B
        background[index + 1U] = 0x66U;  // G
        background[index + 2U] = 0x33U;  // R
        background[index + 3U] = 0xFFU;  // A
    }
    page.raster.pixels = cnote::PixelBuffer(std::move(background));

    cnote::PageText text {};
    text.id = 1;
    text.x = 20.0f;
    text.y = 40.0f;
    text.width = 160.0f;
    text.fontSize = 20.0f;
    text.color = 0xFF000000U;
    text.content = u"colored page text";
    page.texts.push_back(text);

    cnote::PageLatex latex {};
    latex.id = 2;
    latex.x = 20.0f;
    latex.y = 110.0f;
    latex.fontSize = 24.0f;
    latex.color = 0xFF000000U;
    latex.content = u"x^2";
    page.latexes.push_back(latex);

    cnote::Stroke stroke {};
    stroke.style.color = 0xFF00A000U;
    stroke.style.width = 4.0f;
    stroke.points = { { 20.0f, 170.0f }, { 180.0f, 170.0f } };
    page.strokes.push_back(stroke);
    return document;
}

cnote::Page BuildOverlayPage(float width) {
    cnote::Page page;
    page.width = width;
    page.height = 200.0f;

    cnote::PageText text {};
    text.id = 1;
    text.x = 20.5f;
    text.y = 40.25f;
    text.fontSize = 20.0f;
    text.color = 0xFF000000U;
    text.rotationDegrees = 17.0f;
    text.content = u"overlay parity";
    page.texts.push_back(text);

    cnote::PageLatex latex {};
    latex.id = 2;
    latex.x = 30.0f;
    latex.y = 120.0f;
    latex.fontSize = 26.0f;
    latex.color = 0xFF000000U;
    latex.content = u"x^2 + y";
    page.latexes.push_back(latex);
    return page;
}

// The old export path painted the whole overlay into one page-sized image.
// This reproduces that composition (white or transparent background) as the
// reference the cropped rasters are compared against.
QImage PaintOverlayReference(const cnote::Page& page, float scale, bool opaqueBackground) {
    const int width = std::max(1, static_cast<int>(std::lround(page.width * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(page.height * scale)));
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(opaqueBackground ? QColor(Qt::white) : QColor(Qt::transparent));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    for (const cnote::PageText& text : page.texts) {
        cnote::render::PaintPageText(painter, text, page.width, scale, std::nullopt, 1.0f);
    }
    for (const cnote::PageLatex& latex : page.latexes) {
        cnote::render::PaintPageLatex(painter, latex, scale, 1.0f);
    }
    painter.end();
    return image.convertToFormat(QImage::Format_ARGB32);
}

struct ImageDifference {
    std::size_t differingPixels = 0;
    int maxChannelDifference = 0;
};

// Compares two straight-ARGB images channel by channel, so alpha coverage
// (placement and clipping) is exact while the premultiply round trip stays
// within one step.
ImageDifference CompareArgb(const QImage& expected, const QImage& actual) {
    ImageDifference difference;
    if (expected.width() != actual.width() || expected.height() != actual.height()) {
        difference.maxChannelDifference = 255;
        return difference;
    }
    for (int y = 0; y < expected.height(); ++y) {
        const auto* expectedRow = reinterpret_cast<const QRgb*>(expected.constScanLine(y));
        const auto* actualRow = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
        for (int x = 0; x < expected.width(); ++x) {
            bool differs = false;
            const int alphaDifference = std::abs(qAlpha(expectedRow[x]) - qAlpha(actualRow[x]));
            if (alphaDifference != 0) {
                difference.maxChannelDifference = 255;
                differs = true;
            }
            for (const int channelDifference : { std::abs(qRed(expectedRow[x]) - qRed(actualRow[x])),
                     std::abs(qGreen(expectedRow[x]) - qGreen(actualRow[x])),
                     std::abs(qBlue(expectedRow[x]) - qBlue(actualRow[x])) }) {
                difference.maxChannelDifference = std::max(difference.maxChannelDifference, channelDifference);
                if (channelDifference != 0) {
                    differs = true;
                }
            }
            if (differs) {
                ++difference.differingPixels;
            }
        }
    }
    return difference;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    if (!cnote::latex::Initialize(std::filesystem::path(CNOTE_LATEX_RES_ROOT))) {
        std::printf("FAIL: latex engine did not initialize\n");
        return 1;
    }

    const std::filesystem::path dir = std::filesystem::temp_directory_path()
        / ("cnote-pdf-export-tests-" + std::to_string(static_cast<unsigned long long>(std::rand())));
    std::error_code error;
    std::filesystem::create_directories(dir, error);

    cnote::PdfiumBackend backend;

    // --- embedded vs attachment-free export of a synthetic project ----------
    cnote::Document document = BuildSyntheticDocument();
    const std::vector<std::uint8_t> embeddedProject = cnote::ProjectFile::Serialize(document);
    Check(backend.ExportPdf(document, dir / "embedded.pdf", 1.0f, 0.55f, 3.0f, 2.0f, embeddedProject),
        "export with embedded project");
    Check(backend.ExportPdf(document, dir / "flat.pdf", 1.0f, 0.55f, 3.0f, 2.0f, std::span<const std::uint8_t> {}),
        "export without embedded project");

    // A requested embedded project that the attachment writer cannot store
    // must fail the export instead of silently producing an attachment-free
    // PDF that reports success.
    {
        const std::uint8_t byte = 0U;
        const std::span<const std::uint8_t> oversizedProject(
            &byte, static_cast<std::size_t>(std::numeric_limits<int>::max()));
        Check(!backend.ExportPdf(document, dir / "oversized-embedded.pdf", 1.0f, 0.55f, 3.0f, 2.0f,
                oversizedProject),
            "an embedded project the attachment writer rejects fails the export");
        Check(!std::filesystem::exists(dir / "oversized-embedded.pdf"),
            "a failed embedded export writes no destination file");
    }

    auto reopenedEmbedded = cnote::LoadDocumentFile(dir / "embedded.pdf", backend);
    Check(reopenedEmbedded.has_value(), "embedded export reopens");
    if (reopenedEmbedded.has_value()) {
        const cnote::Page& page = reopenedEmbedded->PageAt(0);
        Check(reopenedEmbedded->PageCount() == document.PageCount(), "embedded export keeps the page count");
        Check(page.texts.size() == 1U && page.texts.front().content == u"colored page text",
            "embedded export keeps text content");
        Check(page.latexes.size() == 1U && page.latexes.front().content == u"x^2",
            "embedded export keeps LaTeX content");
        Check(page.strokes.size() == 1U && page.strokes.front().points.size() == 2U,
            "embedded export keeps strokes");
    }

    // PDF-backed pages keep no eager raster; the Qt canvas gets them lazily
    // through PdfBackgroundProvider (covered by pdf_background_qt).
    {
        auto pdfOnly = backend.OpenFromMemory(
            dir / "raster-source.pdf",
            std::make_shared<std::vector<std::uint8_t>>(BuildTextPdf()));
        Check(pdfOnly.has_value() && pdfOnly->PageAt(0).raster.pixels.empty(),
            "PDF-only document does not materialize page rasters eagerly");
    }

    auto reopenedFlat = cnote::LoadDocumentFile(dir / "flat.pdf", backend);
    Check(reopenedFlat.has_value(), "attachment-free export reopens as a readable PDF");
    Check(reopenedFlat.has_value() && reopenedFlat->PageCount() == document.PageCount(),
        "attachment-free export keeps the page count");

    // --- pen ink exports as pressure-width vector ink -----------------------
    // A pressure ramp must come out of the PDF thin at the light end and wide
    // at the heavy end, like it renders on the canvas.
    {
        cnote::Document inkDocument = cnote::Document::CreateBlank();
        cnote::Page& inkPage = inkDocument.CurrentPage();
        inkPage.width = 200.0f;
        inkPage.height = 100.0f;
        cnote::Stroke ramp {};
        ramp.style.color = 0xFF000000U;
        ramp.style.width = 12.0f;
        for (int i = 0; i <= 150; ++i) {
            ramp.points.push_back({ 25.0f + static_cast<float>(i), 50.0f });
            ramp.pressures.push_back(static_cast<float>(i) / 150.0f);
        }
        inkPage.strokes.push_back(ramp);
        Check(backend.ExportPdf(inkDocument, dir / "ink.pdf", 1.0f, 0.55f, 3.0f, 2.0f, std::span<const std::uint8_t> {}),
            "export pen ink");
        const auto reopenedInk = cnote::LoadDocumentFile(dir / "ink.pdf", backend);
        const auto inkRaster = reopenedInk.has_value()
            ? backend.RenderPage(reopenedInk->PdfData(), 0U, 4.0f)
            : std::optional<cnote::RasterImage> {};
        Check(inkRaster.has_value(), "exported pen ink renders");
        if (inkRaster.has_value()) {
            const auto thicknessAt = [&inkRaster](float pageX) {
                const int x = static_cast<int>(pageX * 4.0f);
                int count = 0;
                for (int y = 0; y < inkRaster->height; ++y) {
                    const std::uint8_t* pixel = inkRaster->pixels.data()
                        + (static_cast<std::size_t>(y) * static_cast<std::size_t>(inkRaster->stride))
                        + (static_cast<std::size_t>(x) * 4U);
                    count += pixel[1] < 128 ? 1 : 0;
                }
                return count;
            };
            const int light = thicknessAt(25.0f + 30.0f);
            const int heavy = thicknessAt(25.0f + 120.0f);
            Check(light > 0 && heavy > light * 2,
                "exported pressure widens the ink (light=" + std::to_string(light) + " heavy=" + std::to_string(heavy) + ")");
        }
    }

    // --- the text/LaTeX overlay is one cropped raster per object -----------
    {
        const cnote::Page& overlayPage = document.PageAt(0);
        const auto overlays = cnote::ComposeTextOverlayRasters(overlayPage, 1.0f);
        Check(overlays.size() == 2U, "one overlay raster per painted text/LaTeX object");
        std::size_t totalBytes = 0;
        for (const cnote::PlacedRaster& overlay : overlays) {
            Check(!overlay.image.pixels.empty(), "text/LaTeX overlay composes");
            Check(!overlay.opaque, "text/LaTeX overlay keeps its alpha");
            Check(overlay.image.width < static_cast<int>(overlayPage.width)
                    && overlay.image.height < static_cast<int>(overlayPage.height),
                "overlay is cropped to its object, not the page");
            Check(overlay.left >= 0.0f && overlay.top >= 0.0f
                    && overlay.right <= overlayPage.width && overlay.bottom <= overlayPage.height,
                "overlay placement stays inside the page");
            std::size_t transparent = 0;
            for (std::size_t index = 3; index < overlay.image.pixels.size(); index += 4U) {
                if (overlay.image.pixels.data()[index] == 0) {
                    ++transparent;
                }
            }
            const std::size_t total = overlay.image.pixels.size() / 4U;
            Check(total > 0 && transparent * 2 > total,
                "text/LaTeX overlay is mostly transparent (no opaque rectangle)");
            totalBytes += overlay.image.pixels.size();
        }
        const std::size_t fullPageBytes = static_cast<std::size_t>(overlayPage.width)
            * static_cast<std::size_t>(overlayPage.height) * 4U;
        Check(totalBytes * 2U < fullPageBytes, "overlay bytes stay proportional to painted area (total "
                + std::to_string(totalBytes) + " full " + std::to_string(fullPageBytes) + ")");
    }

    // --- cropped overlays reproduce the old full-page composition -----------
    {
        constexpr float scale = 4.0f;
        const cnote::Page page = BuildOverlayPage(200.0f);
        const auto overlays = cnote::ComposeTextOverlayRasters(page, scale);
        Check(overlays.size() == 2U, "rotated text and LaTeX each compose one raster");

        const int widthPx = static_cast<int>(std::lround(page.width * scale));
        const int heightPx = static_cast<int>(std::lround(page.height * scale));
        QImage placed(widthPx, heightPx, QImage::Format_ARGB32);
        placed.fill(Qt::transparent);
        {
            QPainter painter(&placed);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            for (const cnote::PlacedRaster& overlay : overlays) {
                const int leftPx = static_cast<int>(std::lround(overlay.left * widthPx / page.width));
                const int topPx = static_cast<int>(std::lround(overlay.top * heightPx / page.height));
                const QImage view = cnote::render::WrapBgraImage(
                    overlay.image.pixels.data(),
                    overlay.image.width,
                    overlay.image.height,
                    overlay.image.stride);
                painter.drawImage(QPoint(leftPx, topPx), view);
            }
        }
        const ImageDifference difference = CompareArgb(PaintOverlayReference(page, scale, false), placed);
        Check(difference.maxChannelDifference <= 1,
            "cropped overlays reproduce the full-page overlay pixels (max channel difference "
                + std::to_string(difference.maxChannelDifference) + ")");
    }

    // --- the exported page matches a direct full-page overlay render --------
    {
        constexpr float scale = 4.0f;
        cnote::Document overlayDocument = cnote::Document::CreateBlank();
        overlayDocument.CurrentPage() = BuildOverlayPage(200.0f);
        Check(backend.ExportPdf(overlayDocument, dir / "overlay-parity.pdf", scale, 0.55f, 3.0f, 2.0f,
                std::span<const std::uint8_t> {}),
            "export a document with cropped text/LaTeX overlays");
        auto reopened = cnote::LoadDocumentFile(dir / "overlay-parity.pdf", backend);
        Check(reopened.has_value(), "cropped-overlay export reopens");
        if (reopened.has_value()) {
            const auto rendered = backend.RenderPage(reopened->PdfData(), 0U, scale);
            Check(rendered.has_value(), "cropped-overlay export renders");
            if (rendered.has_value()) {
                const QImage expected = PaintOverlayReference(overlayDocument.PageAt(0), scale, true);
                const QImage actual = cnote::render::WrapBgraImage(
                    rendered->pixels.data(), rendered->width, rendered->height, rendered->stride);
                const ImageDifference difference = CompareArgb(expected, actual);
                const std::size_t totalPixels = static_cast<std::size_t>(expected.width())
                    * static_cast<std::size_t>(expected.height());
                Check(difference.maxChannelDifference <= 1,
                    "exported text/LaTeX placement matches direct rendering (max channel difference "
                        + std::to_string(difference.maxChannelDifference) + ")");
                Check(difference.differingPixels * 100U <= totalPixels,
                    "exported text/LaTeX pixels match direct rendering (differing "
                        + std::to_string(difference.differingPixels) + " of " + std::to_string(totalPixels) + ")");
            }
        }
    }

    // --- an opaque stored raster at export resolution is passed through ------
    {
        constexpr float scale = 4.0f;
        cnote::Document rasterDocument = cnote::Document::CreateBlank();
        cnote::Page& rasterPage = rasterDocument.CurrentPage();
        rasterPage.width = 200.0f;
        rasterPage.height = 120.0f;
        rasterPage.raster.width = 800;
        rasterPage.raster.height = 480;
        rasterPage.raster.stride = 800 * 4;
        rasterPage.raster.scale = scale;
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(rasterPage.raster.stride) * static_cast<std::size_t>(rasterPage.raster.height));
        for (int y = 0; y < rasterPage.raster.height; ++y) {
            for (int x = 0; x < rasterPage.raster.width; ++x) {
                std::uint8_t* pixel = pixels.data()
                    + (static_cast<std::size_t>(y) * static_cast<std::size_t>(rasterPage.raster.stride))
                    + (static_cast<std::size_t>(x) * 4U);
                pixel[0] = static_cast<std::uint8_t>(x * 255 / (rasterPage.raster.width - 1));
                pixel[1] = static_cast<std::uint8_t>(y * 255 / (rasterPage.raster.height - 1));
                pixel[2] = static_cast<std::uint8_t>(((x / 40) + (y / 40)) % 2 == 0 ? 0x20U : 0xE0U);
                pixel[3] = 0xFFU;
            }
        }
        rasterPage.raster.pixels = cnote::PixelBuffer(std::move(pixels));

        Check(backend.ExportPdf(rasterDocument, dir / "stored-raster.pdf", scale, 0.55f, 3.0f, 2.0f,
                std::span<const std::uint8_t> {}),
            "export an opaque stored raster at the export resolution");
        auto reopenedRaster = cnote::LoadDocumentFile(dir / "stored-raster.pdf", backend);
        Check(reopenedRaster.has_value(), "stored-raster export reopens");
        if (reopenedRaster.has_value()) {
            const auto rendered = backend.RenderPage(reopenedRaster->PdfData(), 0U, scale);
            Check(rendered.has_value(), "stored-raster export renders");
            if (rendered.has_value()) {
                const QImage expected = cnote::render::WrapBgraImage(
                    rasterPage.raster.pixels.data(),
                    rasterPage.raster.width,
                    rasterPage.raster.height,
                    rasterPage.raster.stride);
                const QImage actual = cnote::render::WrapBgraImage(
                    rendered->pixels.data(), rendered->width, rendered->height, rendered->stride);
                const ImageDifference difference = CompareArgb(expected, actual);
                Check(difference.maxChannelDifference <= 1,
                    "opaque stored raster keeps its pixels (max channel difference "
                        + std::to_string(difference.maxChannelDifference) + ")");
            }
        }
    }

    // --- selectable source text survives an attachment-free export ----------
    auto pdfDocument = backend.OpenFromMemory(
        dir / "source.pdf",
        std::make_shared<std::vector<std::uint8_t>>(BuildTextPdf()));
    Check(pdfDocument.has_value(), "synthetic source PDF opens");
    if (pdfDocument.has_value()) {
        cnote::PageText overlayText {};
        overlayText.id = 1;
        overlayText.x = 20.0f;
        overlayText.y = 40.0f;
        overlayText.width = 160.0f;
        overlayText.fontSize = 20.0f;
        overlayText.content = u"overlay";
        pdfDocument->PageAt(0).texts.push_back(overlayText);

        Check(backend.ExportPdf(*pdfDocument, dir / "source-flat.pdf", 1.0f, 0.55f, 3.0f, 2.0f, std::span<const std::uint8_t> {}),
            "export source PDF without embedded project");
        auto reopenedSource = cnote::LoadDocumentFile(dir / "source-flat.pdf", backend);
        Check(reopenedSource.has_value(), "source export reopens");
        if (reopenedSource.has_value()) {
            const std::string text = ExtractText(backend, reopenedSource->PdfData(), 0);
            Check(text.find("Hello PDF") != std::string::npos,
                "attachment-free export keeps selectable source text");
        }
    }

    std::filesystem::remove_all(dir, error);
    cnote::latex::Shutdown();
    std::printf(g_failures == 0 ? "ALL PDF EXPORT TESTS PASSED\n" : "%d PDF EXPORT TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
