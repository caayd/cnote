#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Document.h"
#include "PdfBackend.h"
#include "ProjectFile.h"
#include "Unicode.h"
#include "document/SelectionClipboardCodec.h"
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
    text.content = u"Styled text\r\nsecond line";
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
    latex.content = u"\\frac{a}{b}";
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

    const auto loaded = cnote::ProjectFile::LoadFromBytes(bytes, L"text-model-test.cnote");
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
    changedLatex.content = u"\\sqrt{a}";
    if (!Require(!cnote::PageLatexEqual(latex, changedLatex), "latex block changes are not detected")) {
        return 1;
    }

    // Unicode contract: offsets are UTF-16 code units, embedded NUL survives,
    // malformed input is replaced, and CRLF survives save/reopen unedited.
    cnote::PageText unicodeText {};
    unicodeText.id = 44;
    unicodeText.width = 240.0f;
    unicodeText.fontSize = 18.0f;
    unicodeText.content = u"A\U0001F600B\r\n\u03B1";
    unicodeText.styleSpans.push_back({ 3U, 1U, {} });
    cnote::Document unicodeDocument = cnote::Document::CreateBlank();
    unicodeDocument.CurrentPage().texts.push_back(unicodeText);
    const auto unicodeLoaded = cnote::ProjectFile::LoadFromBytes(
        cnote::ProjectFile::Serialize(unicodeDocument),
        L"unicode-test.cnote");
    if (!Require(unicodeLoaded.has_value(), "unicode project did not load")
        || !Require(unicodeLoaded->CurrentPage().texts.size() == 1U, "unicode text count changed")) {
        return 1;
    }
    const cnote::PageText& roundTripped = unicodeLoaded->CurrentPage().texts.front();
    if (!Require(roundTripped.content == u"A\U0001F600B\r\n\u03B1", "unicode content changed during round trip")
        || !Require(roundTripped.content.size() == 7U, "unicode content is not 7 UTF-16 code units")
        || !Require(roundTripped.content[4] == u'\r' && roundTripped.content[5] == u'\n', "CRLF was not preserved")
        || !Require(roundTripped.styleSpans.size() == 1U, "unicode span count changed")
        || !Require(roundTripped.styleSpans.front().start == 3U, "span start is not UTF-16 code unit 3")
        || !Require(roundTripped.styleSpans.front().length == 1U, "span length is not 1 code unit")) {
        return 1;
    }

    cnote::PageText nulText = unicodeText;
    nulText.content.assign(u"A\0B", 3U);
    nulText.styleSpans.clear();
    cnote::Document nulDocument = cnote::Document::CreateBlank();
    nulDocument.CurrentPage().texts.push_back(nulText);
    const auto nulLoaded = cnote::ProjectFile::LoadFromBytes(
        cnote::ProjectFile::Serialize(nulDocument),
        L"nul-test.cnote");
    if (!Require(nulLoaded.has_value() && nulLoaded->CurrentPage().texts.size() == 1U, "embedded NUL project did not load")
        || !Require(nulLoaded->CurrentPage().texts.front().content.size() == 3U, "embedded NUL was truncated")
        || !Require(nulLoaded->CurrentPage().texts.front().content[1] == u'\0', "embedded NUL was not preserved")) {
        return 1;
    }

    // Malformed input is handled without failure: invalid UTF-8 decodes to
    // U+FFFD, and an unpaired surrogate never emits invalid UTF-8 (Qt drops it).
    const std::u16string replaced = cnote::Utf8ToUtf16(std::string("\xC3\x28", 2));
    if (!Require(replaced.size() == 2U, "invalid UTF-8 length changed")
        || !Require(replaced[0] == u'\uFFFD', "invalid UTF-8 was not replaced with U+FFFD")
        || !Require(replaced[1] == u'(', "valid byte after an invalid sequence was dropped")) {
        return 1;
    }
    if (!Require(cnote::Utf16ToUtf8(std::u16string(1, static_cast<char16_t>(0xD800))).empty(),
            "unpaired surrogate emitted invalid UTF-8")) {
        return 1;
    }

    // CTS selection codec: CTS8 round trip with UTF-16LE text, and rejection of
    // malformed payloads so callers leave the document untouched.
    cnote::SelectionClipboardData clipboardData;
    cnote::Stroke clipboardStroke {};
    clipboardStroke.style.color = 0xFF112233U;
    clipboardStroke.style.width = 4.0f;
    clipboardStroke.style.dotted = true;
    clipboardStroke.smoothing = 0.4f;
    clipboardStroke.points = { { 1.0f, 2.0f }, { 3.0f, 4.0f } };
    clipboardData.strokes.push_back(clipboardStroke);
    cnote::PageText clipboardText {};
    clipboardText.x = 5.0f;
    clipboardText.y = 6.0f;
    clipboardText.width = 100.0f;
    clipboardText.height = 20.0f;
    clipboardText.fontSize = 18.0f;
    clipboardText.color = 0xFF445566U;
    clipboardText.bold = true;
    clipboardText.content = u"A\U0001F600";
    clipboardText.styleSpans.push_back({ 1U, 2U, {} });
    clipboardData.texts.push_back(clipboardText);
    cnote::PageLatex clipboardLatex {};
    clipboardLatex.x = 7.0f;
    clipboardLatex.y = 8.0f;
    clipboardLatex.fontSize = 24.0f;
    clipboardLatex.color = 0xFF778899U;
    clipboardLatex.content = u"\\frac{a}{b}";
    clipboardData.latexes.push_back(clipboardLatex);
    cnote::PageImage clipboardImage {};
    clipboardImage.x = 9.0f;
    clipboardImage.y = 10.0f;
    clipboardImage.width = 11.0f;
    clipboardImage.height = 12.0f;
    clipboardImage.rotationDegrees = 13.0f;
    clipboardImage.pixelWidth = 2;
    clipboardImage.pixelHeight = 1;
    clipboardImage.stride = 8;
    clipboardImage.pixels = { 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U };
    clipboardData.images.push_back(clipboardImage);
    clipboardData.minX = 1.0f;
    clipboardData.minY = 2.0f;
    clipboardData.maxX = 20.0f;
    clipboardData.maxY = 30.0f;

    const std::vector<std::uint8_t> clipboardBytes = cnote::EncodeSelectionClipboard(clipboardData);
    if (!Require(clipboardBytes.size() > 4U && std::memcmp(clipboardBytes.data(), "CTS8", 4) == 0,
            "selection payload is not CTS8")) {
        return 1;
    }
    const auto decodedClipboard = cnote::DecodeSelectionClipboard(clipboardBytes);
    if (!Require(decodedClipboard.has_value(), "selection payload did not decode")
        || !Require(decodedClipboard->strokes.size() == 1U && decodedClipboard->texts.size() == 1U
            && decodedClipboard->latexes.size() == 1U && decodedClipboard->images.size() == 1U,
            "selection counts changed")
        || !Require(decodedClipboard->strokes.front().style.dotted
            && decodedClipboard->strokes.front().points.size() == 2U, "selection stroke changed")
        || !Require(decodedClipboard->texts.front().content == u"A\U0001F600", "selection text changed")
        || !Require(decodedClipboard->texts.front().styleSpans.size() == 1U
            && decodedClipboard->texts.front().styleSpans.front().start == 1U
            && decodedClipboard->texts.front().styleSpans.front().length == 2U, "selection span changed")
        || !Require(decodedClipboard->latexes.front().content == u"\\frac{a}{b}", "selection latex changed")
        || !Require(decodedClipboard->images.front().pixels == clipboardImage.pixels, "selection image changed")
        || !Require(decodedClipboard->minX == 1.0f && decodedClipboard->maxY == 30.0f, "selection bounds changed")) {
        return 1;
    }
    if (!Require(!cnote::DecodeSelectionClipboard(std::span<const std::uint8_t>(clipboardBytes.data(), 3U)).has_value(),
            "truncated selection header was accepted")
        || !Require(!cnote::DecodeSelectionClipboard(
                std::span<const std::uint8_t>(clipboardBytes.data(), clipboardBytes.size() - 1U)).has_value(),
            "truncated selection payload was accepted")) {
        return 1;
    }
    const std::uint8_t unknownMagic[4] = { 'X', 'T', 'S', '8' };
    if (!Require(!cnote::DecodeSelectionClipboard(std::span<const std::uint8_t>(unknownMagic, 4U)).has_value(),
            "unknown selection magic was accepted")) {
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
    wrappingText.content = u"the quick brown fox jumps over the lazy dog several times without adding a line";
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
    transparencyText.content = u"Text";
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
