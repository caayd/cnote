#include <iostream>

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
