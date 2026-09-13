#include "App.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "document/SelectionClipboardCodec.h"
#include "text/TextGeometry.h"
#include "latex/LatexEngine.h"

namespace cnote {

namespace textgeom = cnote::text;

namespace {

UINT SelectionClipboardFormat() {
    static const UINT format = RegisterClipboardFormatW(L"cnote.selection.v1");
    return format;
}

}  // namespace

void AppendPdfUnicode(std::u16string& text, std::uint32_t unicode) {
    if (unicode <= 0xFFFFU) {
        text.push_back(static_cast<char16_t>(unicode));
        return;
    }
    if (unicode <= 0x10FFFFU) {
        const std::uint32_t codePoint = unicode - 0x10000U;
        text.push_back(static_cast<char16_t>(0xD800U + (codePoint >> 10U)));
        text.push_back(static_cast<char16_t>(0xDC00U + (codePoint & 0x3FFU)));
    }
}

std::optional<Vec2> App::DefaultPasteOrigin(std::size_t pageIndex) const {
    if (pageIndex >= session_->document.PageCount()) {
        return std::nullopt;
    }

    const Page& page = session_->document.PageAt(pageIndex);
    const Vec2 fallback {
        page.width * 0.5f,
        page.height * 0.5f,
    };

    if (window_ == nullptr) {
        return fallback;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT canvasRect = CanvasRect(clientRect);
    const PageView pageView = CalculatePageView(clientRect, pageIndex);
    if (!pageView.valid || pageView.scale <= 0.0f) {
        return fallback;
    }
    RECT visibleRect {};
    if (IntersectRect(&visibleRect, &pageView.rect, &canvasRect) == 0) {
        return fallback;
    }

    POINT visibleCenter {
        (visibleRect.left + visibleRect.right) / 2,
        (visibleRect.top + visibleRect.bottom) / 2,
    };
    return ScreenToPage(pageView, visibleCenter, true);
}

bool App::CopyPdfTextSelectionToClipboard() {
    std::u16string text;
    const PdfTextCharBox* previousBox = nullptr;
    for (const std::vector<PdfTextCharBox>& boxes : selectedPdfTextBoxes_) {
        for (const PdfTextCharBox& box : boxes) {
            if (previousBox != nullptr) {
                const float previousHeight = previousBox->bottom - previousBox->top;
                const float currentHeight = box.bottom - box.top;
                const float lineBreakThreshold = std::max(previousHeight, currentHeight) * 0.5f;
                if (box.top > previousBox->bottom + lineBreakThreshold
                    || box.left + (box.right - box.left) * 0.5f < previousBox->left) {
                    text.push_back(L'\n');
                }
            }
            AppendPdfUnicode(text, box.unicode);
            previousBox = &box;
        }
    }
    if (text.empty() || window_ == nullptr || !OpenClipboard(window_)) {
        return false;
    }

    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1U) * sizeof(char16_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        CloseClipboard();
        return false;
    }
    void* destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    std::memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

bool App::CopySelectionToClipboard() {
    if (HasSelectedPdfText()) {
        return CopyPdfTextSelectionToClipboard();
    }
    const std::optional<std::size_t> pageIndex = FirstSelectedPageIndex();
    if (!pageIndex.has_value()) {
        return false;
    }

    const Page& page = session_->document.PageAt(*pageIndex);
    std::vector<Stroke> strokes;
    std::vector<PageText> texts;
    std::vector<PageLatex> latexes;
    std::vector<PageImage> images;
    float minX = page.width;
    float minY = page.height;
    float maxX = 0.0f;
    float maxY = 0.0f;
    bool hasContent = false;

    for (std::size_t strokeIndex : selectedStrokeIndices_[*pageIndex]) {
        if (strokeIndex >= page.strokes.size()) {
            continue;
        }
        const Stroke& stroke = page.strokes[strokeIndex];
        strokes.push_back(stroke);
        for (const Vec2& point : stroke.points) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
            hasContent = true;
        }
    }

    for (std::size_t textIndex : selectedTextIndices_[*pageIndex]) {
        if (textIndex >= page.texts.size()) {
            continue;
        }
        const PageText& text = page.texts[textIndex];
        const textgeom::PageTextBounds bounds = textgeom::MeasurePageTextBounds(text, page.width);
        texts.push_back(text);
        minX = std::min(minX, bounds.left);
        minY = std::min(minY, bounds.top);
        maxX = std::max(maxX, bounds.right);
        maxY = std::max(maxY, bounds.bottom);
        hasContent = true;
    }

    for (std::size_t latexIndex : selectedLatexIndices_[*pageIndex]) {
        if (latexIndex >= page.latexes.size()) {
            continue;
        }
        const PageLatex& latexBlock = page.latexes[latexIndex];
        const cnote::latex::FormulaMetrics metrics = cnote::latex::Measure(latexBlock);
        latexes.push_back(latexBlock);
        minX = std::min(minX, latexBlock.x);
        minY = std::min(minY, latexBlock.y);
        maxX = std::max(maxX, latexBlock.x + metrics.width);
        maxY = std::max(maxY, latexBlock.y + metrics.height);
        hasContent = true;
    }

    for (std::size_t imageIndex : selectedImageIndices_[*pageIndex]) {
        if (imageIndex >= page.images.size()) {
            continue;
        }
        const PageImage& image = page.images[imageIndex];
        images.push_back(image);
        minX = std::min(minX, image.x);
        minY = std::min(minY, image.y);
        maxX = std::max(maxX, image.x + image.width);
        maxY = std::max(maxY, image.y + image.height);
        hasContent = true;
    }

    if (!hasContent || !OpenClipboard(window_)) {
        return false;
    }

    SelectionClipboardData data;
    data.strokes = std::move(strokes);
    data.texts = std::move(texts);
    data.latexes = std::move(latexes);
    data.images = std::move(images);
    data.minX = minX;
    data.minY = minY;
    data.maxX = maxX;
    data.maxY = maxY;
    const std::vector<std::uint8_t> bytes = EncodeSelectionClipboard(data);

    EmptyClipboard();
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (memory == nullptr) {
        CloseClipboard();
        return false;
    }

    void* destination = GlobalLock(memory);
    std::copy(bytes.begin(), bytes.end(), static_cast<std::uint8_t*>(destination));
    GlobalUnlock(memory);
    SetClipboardData(SelectionClipboardFormat(), memory);
    CloseClipboard();
    return true;
}

bool App::CutSelectionToClipboard() {
    const std::optional<std::size_t> pageIndex = FirstSelectedPageIndex();
    if (!pageIndex.has_value()) {
        return false;
    }

    if (!CopySelectionToClipboard()) {
        return false;
    }

    DeleteSelection(pageIndex);
    return true;
}

bool App::PasteSelectionFromClipboard() {
    if (!OpenClipboard(window_)) {
        return false;
    }

    HANDLE clipboardData = GetClipboardData(SelectionClipboardFormat());
    if (clipboardData == nullptr) {
        CloseClipboard();
        return false;
    }

    const auto* data = static_cast<const std::uint8_t*>(GlobalLock(clipboardData));
    if (data == nullptr) {
        CloseClipboard();
        return false;
    }

    const std::span<const std::uint8_t> clipboardBytes(
        data,
        static_cast<std::size_t>(GlobalSize(clipboardData)));
    const std::optional<SelectionClipboardData> decoded = DecodeSelectionClipboard(clipboardBytes);
    GlobalUnlock(clipboardData);
    CloseClipboard();
    if (!decoded.has_value()) {
        return false;
    }

    SelectionClipboardData selection = std::move(*decoded);
    std::vector<Stroke> strokes = std::move(selection.strokes);
    std::vector<PageText> texts = std::move(selection.texts);
    std::vector<PageLatex> latexes = std::move(selection.latexes);
    std::vector<PageImage> images = std::move(selection.images);
    const float minX = selection.minX;
    const float minY = selection.minY;
    const float maxX = selection.maxX;
    const float maxY = selection.maxY;

    const std::size_t targetPageIndex = CurrentPageIndex();
    if (targetPageIndex >= session_->document.PageCount()) {
        return false;
    }

    const auto pasteCenter = DefaultPasteOrigin(targetPageIndex);
    if (!pasteCenter.has_value()) {
        return false;
    }

    const Page& targetPage = session_->document.PageAt(targetPageIndex);
    const float contentWidth = std::max(0.0f, maxX - minX);
    const float contentHeight = std::max(0.0f, maxY - minY);
    const Vec2 pasteTopLeft {
        std::clamp(
            pasteCenter->x - (contentWidth * 0.5f),
            0.0f,
            std::max(0.0f, targetPage.width - contentWidth)),
        std::clamp(
            pasteCenter->y - (contentHeight * 0.5f),
            0.0f,
            std::max(0.0f, targetPage.height - contentHeight)),
    };
    const Vec2 delta {
        pasteTopLeft.x - minX,
        pasteTopLeft.y - minY,
    };

    Page& page = session_->document.PageAt(targetPageIndex);
    ClearSelection();
    for (Stroke& stroke : strokes) {
        for (Vec2& point : stroke.points) {
            point.x += delta.x;
            point.y += delta.y;
        }
        page.strokes.push_back(stroke);
        selectedStrokeIndices_[targetPageIndex].push_back(page.strokes.size() - 1U);
    }

    for (PageText& text : texts) {
        text.id = AllocateTextId();
        text.x += delta.x;
        text.y += delta.y;
        page.texts.push_back(text);
        selectedTextIndices_[targetPageIndex].push_back(page.texts.size() - 1U);
    }
    for (PageLatex& latex : latexes) {
        latex.id = AllocateLatexId();
        latex.x += delta.x;
        latex.y += delta.y;
        page.latexes.push_back(latex);
        selectedLatexIndices_[targetPageIndex].push_back(page.latexes.size() - 1U);
    }

    for (PageImage& image : images) {
        image.x += delta.x;
        image.y += delta.y;
        page.images.push_back(image);
        selectedImageIndices_[targetPageIndex].push_back(page.images.size() - 1U);
    }

    SetViewPageIndex(targetPageIndex);
    MarkPdfSidebarCompositionDirty();
    NotifyDocumentMutated();
    InvalidatePageSurfaceCache(targetPageIndex);
    InvalidateCanvas();
    return true;
}

bool App::PasteImageFromClipboard() {
    if (!OpenClipboard(window_)) {
        return false;
    }

    HBITMAP bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    if (bitmap == nullptr) {
        CloseClipboard();
        return false;
    }

    BITMAP sourceInfo {};
    if (GetObjectW(bitmap, sizeof(sourceInfo), &sourceInfo) == 0 || sourceInfo.bmWidth <= 0 || sourceInfo.bmHeight <= 0) {
        CloseClipboard();
        return false;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = sourceInfo.bmWidth;
    bitmapInfo.bmiHeader.biHeight = -sourceInfo.bmHeight;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    const int stride = sourceInfo.bmWidth * 4;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * static_cast<std::size_t>(sourceInfo.bmHeight));
    HDC screenDc = GetDC(nullptr);
    const int rows = GetDIBits(screenDc, bitmap, 0, static_cast<UINT>(sourceInfo.bmHeight), pixels.data(), &bitmapInfo, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screenDc);
    CloseClipboard();
    if (rows == 0) {
        return false;
    }

    const std::size_t targetPageIndex = CurrentPageIndex();
    if (targetPageIndex >= session_->document.PageCount()) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, targetPageIndex);
    const Page& page = session_->document.PageAt(targetPageIndex);
    const float pageScale = std::max(0.1f, pageView.valid ? pageView.scale : 1.0f);

    const float pageWidth = std::min(page.width * 0.75f, static_cast<float>(sourceInfo.bmWidth) / pageScale);
    const float aspect = static_cast<float>(sourceInfo.bmHeight) / std::max(1.0f, static_cast<float>(sourceInfo.bmWidth));
    const float pageHeight = pageWidth * aspect;
    const auto pasteCenter = DefaultPasteOrigin(targetPageIndex);
    if (!pasteCenter.has_value()) {
        return false;
    }

    PageImage image {};
    image.width = pageWidth;
    image.height = pageHeight;
    image.x = std::clamp(pasteCenter->x - (pageWidth * 0.5f), 0.0f, std::max(0.0f, page.width - pageWidth));
    image.y = std::clamp(pasteCenter->y - (pageHeight * 0.5f), 0.0f, std::max(0.0f, page.height - pageHeight));
    image.pixelWidth = sourceInfo.bmWidth;
    image.pixelHeight = sourceInfo.bmHeight;
    image.stride = stride;
    image.pixels = std::move(pixels);

    Page& mutablePage = session_->document.PageAt(targetPageIndex);
    mutablePage.images.push_back(std::move(image));
    ClearSelection();
    selectedImageIndices_[targetPageIndex].push_back(mutablePage.images.size() - 1U);
    SetViewPageIndex(targetPageIndex);
    MarkPdfSidebarCompositionDirty();
    NotifyDocumentMutated();
    InvalidatePageSurfaceCache(targetPageIndex);
    InvalidateCanvas();
    return true;
}

}  // namespace cnote
