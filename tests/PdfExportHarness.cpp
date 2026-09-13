// Qt PDF-export harness (migration plan step 3.4 evidence).
//
// Loads real fixtures, exports with and without an embedded project through the
// Qt overlay path, reopens both, and checks selectable source text, overlay
// transparency over a colored page, and editable-project round trip.
//
// Usage: cnote_pdf_export_harness <fixture-dir> <output-dir>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    } else {
        std::printf("ok:   %s\n", message.c_str());
    }
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

cnote::Document BuildColoredPageDocument() {
    cnote::Document document = cnote::Document::CreateBlank();
    cnote::Page& page = document.CurrentPage();
    page.width = 200.0f;
    page.height = 200.0f;
    page.raster.width = 200;
    page.raster.height = 200;
    page.raster.stride = 200 * 4;
    page.raster.scale = 1.0f;
    page.raster.pixels.assign(static_cast<std::size_t>(page.raster.stride) * 200U, 0);
    for (std::size_t index = 0; index < page.raster.pixels.size(); index += 4U) {
        page.raster.pixels[index + 0U] = 0xCCU;  // B
        page.raster.pixels[index + 1U] = 0x66U;  // G
        page.raster.pixels[index + 2U] = 0x33U;  // R
        page.raster.pixels[index + 3U] = 0xFFU;  // A
    }

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
    return document;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc < 3) {
        std::printf("usage: %s <fixture-dir> <output-dir>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path fixtureDir = argv[1];
    const std::filesystem::path outputDir = argv[2];
    std::error_code error;
    std::filesystem::create_directories(outputDir, error);

    if (!cnote::latex::Initialize(std::filesystem::path(CNOTE_LATEX_RES_ROOT))) {
        std::printf("FAIL: latex engine did not initialize\n");
        return 1;
    }

    cnote::PdfiumBackend backend;

    // --- representative.cnote: embedded and attachment-free export ----------
    auto document = cnote::ProjectFile::Load(fixtureDir / "representative.cnote");
    Check(document.has_value(), "representative.cnote decodes");
    if (!document.has_value()) {
        return 1;
    }
    backend.HydrateProject(*document);
    Check(document->PageCount() == 2U, "representative has two pages");

    const std::vector<std::uint8_t> embeddedProject = cnote::ProjectFile::Serialize(*document);
    Check(backend.ExportPdf(*document, outputDir / "qt-representative-embedded.pdf", 1.0f, 0.55f, 3.0f, 2.0f, embeddedProject),
        "export representative with embedded project");
    Check(backend.ExportPdf(*document, outputDir / "qt-representative-flat.pdf", 1.0f, 0.55f, 3.0f, 2.0f, std::span<const std::uint8_t> {}),
        "export representative without embedded project");

    auto reopenedEmbedded = cnote::LoadDocumentFile(outputDir / "qt-representative-embedded.pdf", backend);
    Check(reopenedEmbedded.has_value(), "embedded PDF reopens");
    Check(reopenedEmbedded.has_value() && reopenedEmbedded->PageAt(0).latexes.size() == 1U
            && reopenedEmbedded->PageAt(0).texts.size() == 1U
            && reopenedEmbedded->PageAt(0).strokes.size() == 3U,
        "embedded PDF reopens as an editable project");

    auto reopenedFlat = cnote::LoadDocumentFile(outputDir / "qt-representative-flat.pdf", backend);
    Check(reopenedFlat.has_value() && reopenedFlat->PageCount() == 2U, "attachment-free PDF reopens with both pages");
    if (reopenedFlat.has_value()) {
        const std::string text = ExtractText(backend, reopenedFlat->PdfData(), 0);
        std::printf("flat page 0 text: %s\n", text.c_str());
        Check(!text.empty(), "attachment-free PDF keeps selectable source text");
        Check(text.find("Reference PDF page 1: selectable source text") != std::string::npos,
            "attachment-free PDF text matches the known source string");
    }

    // --- overlay transparency over a colored page ---------------------------
    const auto overlay = cnote::ComposeTextOverlayRaster(document->PageAt(0), 1.0f);
    Check(overlay.has_value(), "text/LaTeX overlay composed");
    if (overlay.has_value()) {
        std::size_t transparent = 0;
        for (std::size_t index = 3; index < overlay->pixels.size(); index += 4U) {
            if (overlay->pixels[index] == 0) {
                ++transparent;
            }
        }
        const std::size_t total = overlay->pixels.size() / 4U;
        Check(total > 0 && transparent * 2 > total,
            "text/LaTeX overlay is mostly transparent (no opaque rectangle)");
    }

    cnote::Document colored = BuildColoredPageDocument();
    Check(backend.ExportPdf(colored, outputDir / "qt-colored-flat.pdf", 1.0f, 0.55f, 3.0f, 2.0f, std::span<const std::uint8_t> {}),
        "export colored-page document");
    auto reopenedColored = cnote::LoadDocumentFile(outputDir / "qt-colored-flat.pdf", backend);
    Check(reopenedColored.has_value(), "colored-page PDF reopens");
    if (reopenedColored.has_value()) {
        const auto raster = backend.RenderPage(reopenedColored->PdfData(), 0, 1.0f);
        Check(raster.has_value(), "colored-page PDF renders");
        if (raster.has_value()) {
            // A pixel inside the text box but between glyphs must keep the page
            // color; an opaque overlay would have replaced it.
            const std::size_t stride = static_cast<std::size_t>(raster->stride);
            const std::size_t offset = (60U * stride) + (180U * 4U);
            const std::uint8_t blue = raster->pixels[offset + 0U];
            const std::uint8_t green = raster->pixels[offset + 1U];
            const std::uint8_t red = raster->pixels[offset + 2U];
            Check(blue > 150U && green > 60U && red < 120U,
                "colored page background survives under the text/LaTeX overlay");
        }
    }

    // --- 11 MB native fixture ----------------------------------------------
    auto large = cnote::ProjectFile::Load(fixtureDir / "large-native-raw.cnote");
    Check(large.has_value(), "large native fixture decodes");
    if (large.has_value()) {
        backend.HydrateProject(*large);
        Check(large->PageCount() == 24U, "large fixture has 24 pages");
        Check(backend.ExportPdf(*large, outputDir / "qt-large-embedded.pdf", 1.0f, 0.55f, 3.0f, 2.0f, cnote::ProjectFile::Serialize(*large)),
            "export large fixture with embedded project");
        Check(backend.ExportPdf(*large, outputDir / "qt-large-flat.pdf", 1.0f, 0.55f, 3.0f, 2.0f, std::span<const std::uint8_t> {}),
            "export large fixture without embedded project");
        auto largeReopened = cnote::LoadDocumentFile(outputDir / "qt-large-embedded.pdf", backend);
        Check(largeReopened.has_value() && largeReopened->PageCount() == 24U,
            "large embedded PDF reopens as an editable 24-page project");
        auto largeFlat = cnote::LoadDocumentFile(outputDir / "qt-large-flat.pdf", backend);
        Check(largeFlat.has_value() && largeFlat->PageCount() == 24U, "large attachment-free PDF reopens");
    }

    cnote::latex::Shutdown();
    std::printf(g_failures == 0 ? "ALL PDF EXPORT CHECKS PASSED\n" : "%d PDF EXPORT CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
