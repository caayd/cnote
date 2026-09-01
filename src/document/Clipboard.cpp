#include "App.h"

#include <algorithm>
#include <cstring>

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

std::optional<Vec2> App::DefaultPasteOrigin(std::size_t pageIndex) const {
    if (pageIndex >= document_.PageCount()) {
        return std::nullopt;
    }

    const Page& page = document_.PageAt(pageIndex);
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

bool App::CopySelectionToClipboard() {
    const std::optional<std::size_t> pageIndex = FirstSelectedPageIndex();
    if (!pageIndex.has_value()) {
        return false;
    }

    const Page& page = document_.PageAt(*pageIndex);
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

    std::vector<std::uint8_t> bytes;
    const auto appendBytes = [&](const void* data, std::size_t size) {
        const auto* source = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), source, source + size);
    };
    const auto appendUint32 = [&](std::uint32_t value) { appendBytes(&value, sizeof(value)); };
    const auto appendFloat = [&](float value) { appendBytes(&value, sizeof(value)); };

    constexpr std::uint32_t kClipboardStrokeFlagDotted = 1U << 0U;
    const char magic[] = { 'C', 'T', 'S', '8' };
    appendBytes(magic, sizeof(magic));
    appendUint32(static_cast<std::uint32_t>(strokes.size()));
    appendUint32(static_cast<std::uint32_t>(texts.size()));
    appendUint32(static_cast<std::uint32_t>(images.size()));
    appendFloat(minX);
    appendFloat(minY);
    appendFloat(maxX);
    appendFloat(maxY);

    for (const Stroke& stroke : strokes) {
        std::uint32_t strokeFlags = 0;
        if (stroke.style.dotted) {
            strokeFlags |= kClipboardStrokeFlagDotted;
        }
        appendUint32(stroke.style.color);
        appendFloat(stroke.style.width);
        appendUint32(strokeFlags);
        appendFloat(stroke.smoothing);
        appendUint32(static_cast<std::uint32_t>(stroke.points.size()));
        for (const Vec2& point : stroke.points) {
            appendFloat(point.x);
            appendFloat(point.y);
        }
    }

    for (const PageText& text : texts) {
        std::uint32_t flags = 0;
        flags |= static_cast<std::uint32_t>(text.alignment);
        flags |= static_cast<std::uint32_t>(text.fontFamily) << 8U;
        if (text.bold) {
            flags |= 1U << 16U;
        }
        if (text.italic) {
            flags |= 1U << 17U;
        }
        if (text.underline) {
            flags |= 1U << 18U;
        }
        appendFloat(text.x);
        appendFloat(text.y);
        appendFloat(text.width);
        appendFloat(text.height);
        appendFloat(text.fontSize);
        appendUint32(text.color);
        appendUint32(flags);
        appendUint32(static_cast<std::uint32_t>(text.content.size()));
        appendBytes(text.content.data(), text.content.size() * sizeof(wchar_t));
        appendUint32(static_cast<std::uint32_t>(text.styleSpans.size()));
        for (const PageTextStyleSpan& span : text.styleSpans) {
            std::uint32_t spanFlags = static_cast<std::uint32_t>(span.style.fontFamily);
            spanFlags |= span.style.bold ? 1U << 8U : 0U;
            spanFlags |= span.style.italic ? 1U << 9U : 0U;
            spanFlags |= span.style.underline ? 1U << 10U : 0U;
            appendUint32(span.start);
            appendUint32(span.length);
            appendFloat(span.style.fontSize);
            appendUint32(span.style.color);
            appendUint32(spanFlags);
        }
    }

    for (const PageImage& image : images) {
        appendFloat(image.x);
        appendFloat(image.y);
        appendFloat(image.width);
        appendFloat(image.height);
        appendFloat(image.rotationDegrees);
        appendUint32(static_cast<std::uint32_t>(image.pixelWidth));
        appendUint32(static_cast<std::uint32_t>(image.pixelHeight));
        appendUint32(static_cast<std::uint32_t>(image.stride));
        appendUint32(static_cast<std::uint32_t>(image.pixels.size()));
        if (!image.pixels.empty()) {
            appendBytes(image.pixels.data(), image.pixels.size());
        }
    }

    appendUint32(static_cast<std::uint32_t>(latexes.size()));
    for (const PageLatex& latex : latexes) {
        appendFloat(latex.x);
        appendFloat(latex.y);
        appendFloat(latex.fontSize);
        appendUint32(latex.color);
        appendUint32(static_cast<std::uint32_t>(latex.content.size()));
        if (!latex.content.empty()) {
            appendBytes(latex.content.data(), latex.content.size() * sizeof(wchar_t));
        }
    }

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

    SIZE_T size = GlobalSize(clipboardData);
    const auto readExact = [&](std::size_t offset, void* output, std::size_t length) -> bool {
        if (offset + length > static_cast<std::size_t>(size)) {
            return false;
        }
        std::memcpy(output, data + offset, length);
        return true;
    };

    std::size_t offset = 0;
    char magic[4] {};
    if (!readExact(offset, magic, sizeof(magic))
        || (std::memcmp(magic, "CTS1", 4) != 0
            && std::memcmp(magic, "CTS2", 4) != 0
            && std::memcmp(magic, "CTS3", 4) != 0
            && std::memcmp(magic, "CTS4", 4) != 0
            && std::memcmp(magic, "CTS5", 4) != 0
            && std::memcmp(magic, "CTS6", 4) != 0
            && std::memcmp(magic, "CTS7", 4) != 0
            && std::memcmp(magic, "CTS8", 4) != 0)) {
        GlobalUnlock(clipboardData);
        CloseClipboard();
        return false;
    }
    offset += sizeof(magic);

    auto readUint32 = [&](std::uint32_t& value) -> bool {
        const bool ok = readExact(offset, &value, sizeof(value));
        offset += ok ? sizeof(value) : 0U;
        return ok;
    };
    auto readFloat = [&](float& value) -> bool {
        const bool ok = readExact(offset, &value, sizeof(value));
        offset += ok ? sizeof(value) : 0U;
        return ok;
    };

    std::uint32_t strokeCount = 0;
    std::uint32_t textCount = 0;
    std::uint32_t imageCount = 0;
    std::uint32_t latexCount = 0;
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    bool headerOk = readUint32(strokeCount);
    if (headerOk && (std::memcmp(magic, "CTS2", 4) == 0 || std::memcmp(magic, "CTS3", 4) == 0
        || std::memcmp(magic, "CTS4", 4) == 0 || std::memcmp(magic, "CTS5", 4) == 0
        || std::memcmp(magic, "CTS6", 4) == 0 || std::memcmp(magic, "CTS7", 4) == 0
        || std::memcmp(magic, "CTS8", 4) == 0)) {
        headerOk = readUint32(textCount);
    }
    headerOk = headerOk && readUint32(imageCount)
        && readFloat(minX) && readFloat(minY) && readFloat(maxX) && readFloat(maxY);
    if (!headerOk) {
        GlobalUnlock(clipboardData);
        CloseClipboard();
        return false;
    }

    std::vector<Stroke> strokes;
    std::vector<PageText> texts;
    std::vector<PageLatex> latexes;
    std::vector<PageImage> images;
    strokes.reserve(strokeCount);
    texts.reserve(textCount);
    images.reserve(imageCount);

    bool ok = true;
    constexpr std::uint32_t kClipboardStrokeFlagDotted = 1U << 0U;
    for (std::uint32_t strokeIndex = 0; ok && strokeIndex < strokeCount; ++strokeIndex) {
        Stroke stroke {};
        std::uint32_t pointCount = 0;
        std::uint32_t strokeFlags = 0;
        ok = readUint32(stroke.style.color) && readFloat(stroke.style.width);
        if (ok && (std::memcmp(magic, "CTS6", 4) == 0 || std::memcmp(magic, "CTS7", 4) == 0 || std::memcmp(magic, "CTS8", 4) == 0)) {
            ok = readUint32(strokeFlags);
        }
        stroke.style.dotted = (strokeFlags & kClipboardStrokeFlagDotted) != 0U;
        if (ok && (std::memcmp(magic, "CTS7", 4) == 0 || std::memcmp(magic, "CTS8", 4) == 0)) {
            ok = readFloat(stroke.smoothing);
        }
        ok = ok && readUint32(pointCount);
        stroke.points.reserve(pointCount);
        for (std::uint32_t pointIndex = 0; ok && pointIndex < pointCount; ++pointIndex) {
            Vec2 point {};
            ok = readFloat(point.x) && readFloat(point.y);
            if (ok) {
                stroke.points.push_back(point);
            }
        }
        if (ok) {
            strokes.push_back(std::move(stroke));
        }
    }

    for (std::uint32_t textIndex = 0; ok && textIndex < textCount; ++textIndex) {
        PageText text {};
        std::uint32_t charCount = 0;
        std::uint32_t flags = 0;
        ok = readFloat(text.x) && readFloat(text.y);
        if (ok && (std::memcmp(magic, "CTS3", 4) == 0 || std::memcmp(magic, "CTS4", 4) == 0 || std::memcmp(magic, "CTS5", 4) == 0 || std::memcmp(magic, "CTS6", 4) == 0 || std::memcmp(magic, "CTS7", 4) == 0 || std::memcmp(magic, "CTS8", 4) == 0)) {
            ok = readFloat(text.width);
        } else {
            text.width = 0.0f;
        }
        if (ok && (std::memcmp(magic, "CTS4", 4) == 0 || std::memcmp(magic, "CTS5", 4) == 0 || std::memcmp(magic, "CTS6", 4) == 0 || std::memcmp(magic, "CTS7", 4) == 0 || std::memcmp(magic, "CTS8", 4) == 0)) {
            ok = readFloat(text.height);
        } else {
            text.height = 0.0f;
        }
        ok = ok && readFloat(text.fontSize) && readUint32(text.color);
        if (ok && (std::memcmp(magic, "CTS5", 4) == 0 || std::memcmp(magic, "CTS6", 4) == 0 || std::memcmp(magic, "CTS7", 4) == 0 || std::memcmp(magic, "CTS8", 4) == 0)) {
            ok = readUint32(flags);
        }
        ok = ok && readUint32(charCount);
        if (ok) {
            const std::size_t byteCount = static_cast<std::size_t>(charCount) * sizeof(wchar_t);
            text.content.resize(charCount);
            ok = readExact(offset, text.content.data(), byteCount);
            offset += ok ? byteCount : 0U;
        }
        text.alignment = static_cast<TextAlignment>(std::min<std::uint32_t>(flags & 0xFFU, 2U));
        text.fontFamily = static_cast<TextFontFamily>(std::min<std::uint32_t>((flags >> 8U) & 0xFFU, 2U));
        text.bold = (flags & (1U << 16U)) != 0;
        text.italic = (flags & (1U << 17U)) != 0;
        text.underline = (flags & (1U << 18U)) != 0;
        if (ok && std::memcmp(magic, "CTS8", 4) == 0) {
            std::uint32_t spanCount = 0;
            ok = readUint32(spanCount) && spanCount <= text.content.size() + 1U;
            for (std::uint32_t spanIndex = 0; ok && spanIndex < spanCount; ++spanIndex) {
                PageTextStyleSpan span {};
                std::uint32_t spanFlags = 0;
                ok = readUint32(span.start)
                    && readUint32(span.length)
                    && readFloat(span.style.fontSize)
                    && readUint32(span.style.color)
                    && readUint32(spanFlags)
                    && span.start <= text.content.size()
                    && span.length <= text.content.size() - span.start;
                span.style.fontFamily = static_cast<TextFontFamily>(std::min<std::uint32_t>(spanFlags & 0xFFU, 2U));
                span.style.bold = (spanFlags & (1U << 8U)) != 0U;
                span.style.italic = (spanFlags & (1U << 9U)) != 0U;
                span.style.underline = (spanFlags & (1U << 10U)) != 0U;
                if (ok) {
                    text.styleSpans.push_back(std::move(span));
                }
            }
        }
        if (ok) {
            texts.push_back(std::move(text));
        }
    }

    for (std::uint32_t imageIndex = 0; ok && imageIndex < imageCount; ++imageIndex) {
        PageImage image {};
        std::uint32_t pixelWidth = 0;
        std::uint32_t pixelHeight = 0;
        std::uint32_t stride = 0;
        std::uint32_t dataSize = 0;
        ok = readFloat(image.x) && readFloat(image.y) && readFloat(image.width) && readFloat(image.height)
            && readFloat(image.rotationDegrees)
            && readUint32(pixelWidth) && readUint32(pixelHeight) && readUint32(stride) && readUint32(dataSize);
        image.pixelWidth = static_cast<int>(pixelWidth);
        image.pixelHeight = static_cast<int>(pixelHeight);
        image.stride = static_cast<int>(stride);
        if (ok) {
            image.pixels.resize(dataSize);
            ok = readExact(offset, image.pixels.data(), dataSize);
            offset += ok ? dataSize : 0U;
        }
        if (ok) {
            images.push_back(std::move(image));
        }
    }
    if (ok && std::memcmp(magic, "CTS8", 4) == 0
        && offset + sizeof(std::uint32_t) <= static_cast<std::size_t>(size)) {
        ok = readUint32(latexCount);
        latexes.reserve(latexCount);
        for (std::uint32_t latexIndex = 0; ok && latexIndex < latexCount; ++latexIndex) {
            PageLatex latex {};
            std::uint32_t charCount = 0;
            ok = readFloat(latex.x)
                && readFloat(latex.y)
                && readFloat(latex.fontSize)
                && readUint32(latex.color)
                && readUint32(charCount);
            if (ok) {
                const std::size_t byteCount = static_cast<std::size_t>(charCount) * sizeof(wchar_t);
                latex.content.resize(charCount);
                ok = readExact(offset, latex.content.data(), byteCount);
                offset += ok ? byteCount : 0U;
            }
            if (ok) {
                latexes.push_back(std::move(latex));
            }
        }
    }

    GlobalUnlock(clipboardData);
    CloseClipboard();
    if (!ok) {
        return false;
    }

    const std::size_t targetPageIndex = document_.CurrentPageIndex();
    if (targetPageIndex >= document_.PageCount()) {
        return false;
    }

    const auto pasteCenter = DefaultPasteOrigin(targetPageIndex);
    if (!pasteCenter.has_value()) {
        return false;
    }

    const Page& targetPage = document_.PageAt(targetPageIndex);
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

    Page& page = document_.PageAt(targetPageIndex);
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

    document_.SetCurrentPageIndex(targetPageIndex);
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

    const std::size_t targetPageIndex = document_.CurrentPageIndex();
    if (targetPageIndex >= document_.PageCount()) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, targetPageIndex);
    const Page& page = document_.PageAt(targetPageIndex);
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

    Page& mutablePage = document_.PageAt(targetPageIndex);
    mutablePage.images.push_back(std::move(image));
    ClearSelection();
    selectedImageIndices_[targetPageIndex].push_back(mutablePage.images.size() - 1U);
    document_.SetCurrentPageIndex(targetPageIndex);
    MarkPdfSidebarCompositionDirty();
    NotifyDocumentMutated();
    InvalidatePageSurfaceCache(targetPageIndex);
    InvalidateCanvas();
    return true;
}

}  // namespace cnote
