// Self-contained Qt PDF-export contract test (migration plan step 3.4).
//
// Builds synthetic documents, exports embedded and attachment-free PDFs through
// the real service path, reopens them, and defends the export invariants. Needs
// no external fixtures; the selectable-text case embeds a minimal PDF.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <QGuiApplication>

#include "Document.h"
#include "PdfBackend.h"
#include "PdfOverlay.h"
#include "ProjectFile.h"
#include "document/FileIO.h"
#include "latex/LatexEngine.h"

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
    text.height = 40.0f;
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

    // --- the text/LaTeX overlay must not be an opaque rectangle -------------
    const auto overlay = cnote::ComposeTextOverlayRaster(document.PageAt(0), 1.0f);
    Check(overlay.has_value(), "text/LaTeX overlay composes");
    if (overlay.has_value()) {
        std::size_t transparent = 0;
        for (std::size_t index = 3; index < overlay->pixels.size(); index += 4U) {
            if (overlay->pixels.data()[index] == 0) {
                ++transparent;
            }
        }
        const std::size_t total = overlay->pixels.size() / 4U;
        Check(total > 0 && transparent * 2 > total,
            "text/LaTeX overlay is mostly transparent (no opaque rectangle)");
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
        overlayText.height = 40.0f;
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
