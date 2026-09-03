#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "Document.h"
#include "PdfBackend.h"
#include "ProjectFile.h"
#include "text/TextGeometry.h"

namespace {

bool Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

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

}  // namespace

int main() {
    cnote::Document document = cnote::Document::CreateBlank();
    cnote::PageText text {};
    text.id = 42;
    text.x = 24.0f;
    text.y = 36.0f;
    text.width = 240.0f;
    text.fontSize = 18.0f;
    text.content = L"Styled text\r\nsecond line";
    text.styleSpans.push_back({
        0,
        6,
        {
            26.0f,
            0xFF2563EBU,
            cnote::TextFontFamily::Serif,
            true,
            true,
            false,
        },
    });
    document.CurrentPage().texts.push_back(text);
    cnote::PageLatex latex {};
    latex.id = 43;
    latex.x = 48.0f;
    latex.y = 72.0f;
    latex.fontSize = 30.0f;
    latex.content = L"\\frac{a}{b}";
    document.CurrentPage().latexes.push_back(latex);
    const std::vector<std::uint8_t> bytes = cnote::ProjectFile::Serialize(document);
    if (!Require(!bytes.empty(), "serialized project is empty")) {
        return 1;
    }

    cnote::PdfiumBackend pdfBackend;
    const auto pdfBytes = std::make_shared<std::vector<std::uint8_t>>(BuildTextPdf());
    const auto pdfDocument = pdfBackend.OpenFromMemory(L"detected-text-test.pdf", pdfBytes);
    if (!Require(pdfDocument.has_value(), "synthetic PDF did not open")) {
        return 1;
    }
    const auto pdfTextBoxes = pdfBackend.ExtractTextCharBoxes(pdfBytes, 0);
    if (!Require(pdfTextBoxes.has_value(), "PDF text extraction failed")
        || !Require(pdfTextBoxes->size() >= 9U, "PDF text boxes were not detected")) {
        return 1;
    }
    for (const cnote::PdfTextCharBox& box : *pdfTextBoxes) {
        if (!Require(box.left < box.right && box.top < box.bottom, "PDF text box coordinates were not normalized")) {
            return 1;
        }
    }

    const auto loaded = cnote::ProjectFile::LoadFromBytes(bytes, L"text-model-test.cnote", pdfBackend);
    if (!Require(loaded.has_value(), "serialized project did not load")
        || !Require(loaded->PageCount() == 1U, "page count changed")
        || !Require(loaded->CurrentPage().texts.size() == 1U, "text count changed")
        || !Require(loaded->CurrentPage().latexes.size() == 1U, "latex block count changed")
        || !Require(cnote::PageTextEqual(text, loaded->CurrentPage().texts.front()), "text block changed during round trip")
        || !Require(cnote::PageLatexEqual(latex, loaded->CurrentPage().latexes.front()), "latex block changed during round trip")) {
        return 1;
    }

    cnote::PageText changed = text;
    changed.styleSpans.front().style.bold = false;
    if (!Require(!cnote::PageTextEqual(text, changed), "style span changes are not detected")) {
        return 1;
    }

    cnote::PageLatex changedLatex = latex;
    changedLatex.content = L"\\sqrt{a}";
    if (!Require(!cnote::PageLatexEqual(latex, changedLatex), "latex block changes are not detected")) {
        return 1;
    }

    const cnote::text::WrappedTextLayout layout = cnote::text::LayoutPageText(text, document.CurrentPage().width);
    if (!Require(layout.lines.size() >= 2U, "canonical layout lost a paragraph break")
        || !Require(layout.height >= layout.lineHeight * 2, "canonical layout height does not cover its lines")) {
        return 1;
    }

    cnote::PageText wrappingText {};
    wrappingText.width = 90.0f;
    wrappingText.fontSize = 12.0f;
    wrappingText.content = L"the quick brown fox jumps over the lazy dog several times without adding a line";
    const cnote::text::WrappedTextLayout narrowLayout = cnote::text::LayoutPageText(
        wrappingText,
        document.CurrentPage().width);
    wrappingText.width = 120.0f;
    const cnote::text::WrappedTextLayout wideLayout = cnote::text::LayoutPageText(
        wrappingText,
        document.CurrentPage().width);
    if (!Require(wideLayout.height <= narrowLayout.height, "widening text increased its layout height")) {
        return 1;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = 320;
    bitmapInfo.bmiHeader.biHeight = -120;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC screenDc = GetDC(nullptr);
    HDC renderDc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, screenDc);
    if (!Require(renderDc != nullptr && bitmap != nullptr && pixels != nullptr, "could not create text render surface")) {
        return 1;
    }
    HGDIOBJ previousBitmap = SelectObject(renderDc, bitmap);
    RECT surfaceRect { 0, 0, 320, 120 };
    HBRUSH background = CreateSolidBrush(RGB(17, 83, 149));
    FillRect(renderDc, &surfaceRect, background);
    DeleteObject(background);
    cnote::PageText transparencyText = text;
    transparencyText.content = L"Text";
    transparencyText.styleSpans.clear();
    transparencyText.width = 280.0f;
    static_cast<void>(cnote::text::DrawPageText(renderDc, transparencyText, 320.0f, 1.0f, 0, 0));
    const auto* renderedPixels = static_cast<const std::uint32_t*>(pixels);
    const std::uint32_t untouchedPixel = renderedPixels[(10 * 320) + 260] & 0x00FFFFFFU;
    SelectObject(renderDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(renderDc);
    if (!Require(untouchedPixel == 0x00115395U, "text renderer painted an opaque bounding-box background")) {
        return 1;
    }

    return 0;
}
