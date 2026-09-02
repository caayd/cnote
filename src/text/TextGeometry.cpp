#include "text/TextGeometry.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <deque>

#include <richedit.h>

namespace cnote::text {

constexpr float kTextLayoutScale = 8.0f;

void HashLayoutValue(std::uint64_t& hash, std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
}

std::uint64_t TextLayoutSignature(const PageText& text, float pageWidth) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto hashFloat = [&](float value) { HashLayoutValue(hash, std::bit_cast<std::uint32_t>(value)); };
    hashFloat(pageWidth);
    hashFloat(text.width);
    hashFloat(text.height);
    hashFloat(text.fontSize);
    HashLayoutValue(hash, text.color);
    HashLayoutValue(hash, static_cast<std::uint64_t>(text.alignment));
    HashLayoutValue(hash, static_cast<std::uint64_t>(text.fontFamily));
    HashLayoutValue(hash, text.bold);
    HashLayoutValue(hash, text.italic);
    HashLayoutValue(hash, text.underline);
    for (wchar_t ch : text.content) {
        HashLayoutValue(hash, static_cast<std::uint16_t>(ch));
    }
    for (const PageTextStyleSpan& span : text.styleSpans) {
        HashLayoutValue(hash, span.start);
        HashLayoutValue(hash, span.length);
        hashFloat(span.style.fontSize);
        HashLayoutValue(hash, span.style.color);
        HashLayoutValue(hash, static_cast<std::uint64_t>(span.style.fontFamily));
        HashLayoutValue(hash, span.style.bold);
        HashLayoutValue(hash, span.style.italic);
        HashLayoutValue(hash, span.style.underline);
    }
    return hash;
}

HMODULE EnsureRichEditModule() {
    static HMODULE richEditModule = LoadLibraryW(L"Msftedit.dll");
    return richEditModule;
}

HWND RichTextLayoutWindow() {
    static HWND layoutWindow = EnsureRichEditModule() != nullptr
        ? CreateWindowExW(
            0,
            L"RICHEDIT50W",
            L"",
            WS_CHILD | ES_MULTILINE | ES_WANTRETURN,
            0,
            0,
            1,
            1,
            HWND_MESSAGE,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr)
        : nullptr;
    return layoutWindow;
}

HBITMAP CreateTextRenderBitmap(HDC dc, int width, int height, void** bits) {
    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS, bits, nullptr, 0);
}

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect { left, top, right, bottom };
    return rect;
}

RECT MakeRectFromFloatBounds(float left, float top, float right, float bottom) {
    return MakeRect(
        static_cast<int>(std::floor(left)),
        static_cast<int>(std::floor(top)),
        static_cast<int>(std::ceil(right)),
        static_cast<int>(std::ceil(bottom)));
}

float CrossProduct(Vec2 a, Vec2 b, Vec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool SegmentsIntersect(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2) {
    const float d1 = CrossProduct(a1, a2, b1);
    const float d2 = CrossProduct(a1, a2, b2);
    const float d3 = CrossProduct(b1, b2, a1);
    const float d4 = CrossProduct(b1, b2, a2);

    if (((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f))
        && ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f))) {
        return true;
    }

    return false;
}

bool PointInPolygon(Vec2 point, const std::vector<Vec2>& polygon) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Vec2& pi = polygon[i];
        const Vec2& pj = polygon[j];
        const bool intersects = ((pi.y > point.y) != (pj.y > point.y))
            && (point.x < (pj.x - pi.x) * (point.y - pi.y) / ((pj.y - pi.y) + 0.00001f) + pi.x);
        if (intersects) {
            inside = !inside;
        }
    }
    return inside;
}

const wchar_t* PageTextFontFace(TextFontFamily family) {
    switch (family) {
    case TextFontFamily::Serif:
        return L"Times New Roman";
    case TextFontFamily::Monospace:
        return L"Consolas";
    case TextFontFamily::Sans:
    default:
        return L"Segoe UI";
    }
}

int PageTextFontWeight(const PageText& text) {
    return text.bold ? FW_BOLD : FW_NORMAL;
}

int PageTextRenderPaddingPixels(const PageText& text, float scale) {
    return std::max(2, static_cast<int>(std::ceil(std::max(1.0f, text.fontSize) * std::max(0.1f, scale) * 0.12f)));
}

std::wstring TrimTrailingLayoutWhitespace(std::wstring value) {
    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) {
        value.pop_back();
    }
    return value;
}

PageTextStyle DefaultPageTextStyle(const PageText& text) {
    return {
        text.fontSize,
        text.color,
        text.fontFamily,
        text.bold,
        text.italic,
        text.underline,
    };
}

void ApplyPageTextFormatting(HWND editWindow, const PageText& text, float scale, float opacity) {
    if (editWindow == nullptr) {
        return;
    }

    HDC dc = GetDC(editWindow);
    const int dpiY = dc != nullptr ? std::max(1, GetDeviceCaps(dc, LOGPIXELSY)) : 96;
    if (dc != nullptr) {
        ReleaseDC(editWindow, dc);
    }
    const float renderScale = std::max(0.1f, scale);
    const float clampedOpacity = std::clamp(opacity, 0.0f, 1.0f);
    const auto applyRange = [&](LONG start, LONG end, const PageTextStyle& style) {
        CHARRANGE range { start, end };
        SendMessageW(editWindow, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
        CHARFORMAT2W format {};
        format.cbSize = sizeof(format);
        format.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE;
        format.dwEffects = (style.bold ? CFE_BOLD : 0U)
            | (style.italic ? CFE_ITALIC : 0U)
            | (style.underline ? CFE_UNDERLINE : 0U);
        const int pixelHeight = std::max(1, static_cast<int>(std::lround(std::max(1.0f, style.fontSize) * renderScale)));
        format.yHeight = MulDiv(pixelHeight, 72 * 20, dpiY);
        const auto blendChannel = [clampedOpacity](BYTE channel) {
            return static_cast<BYTE>(std::lround((static_cast<float>(channel) * clampedOpacity) + (255.0f * (1.0f - clampedOpacity))));
        };
        format.crTextColor = RGB(
            blendChannel(static_cast<BYTE>((style.color >> 16U) & 0xFFU)),
            blendChannel(static_cast<BYTE>((style.color >> 8U) & 0xFFU)),
            blendChannel(static_cast<BYTE>(style.color & 0xFFU)));
        lstrcpynW(format.szFaceName, PageTextFontFace(style.fontFamily), LF_FACESIZE);
        SendMessageW(editWindow, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
    };

    SendMessageW(editWindow, WM_SETREDRAW, FALSE, 0);
    CHARRANGE originalSelection {};
    SendMessageW(editWindow, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&originalSelection));
    applyRange(0, -1, DefaultPageTextStyle(text));
    for (const PageTextStyleSpan& span : text.styleSpans) {
        const LONG start = static_cast<LONG>(std::min<std::size_t>(span.start, text.content.size()));
        const LONG end = static_cast<LONG>(std::min<std::size_t>(text.content.size(), static_cast<std::size_t>(span.start) + span.length));
        if (end > start) {
            applyRange(start, end, span.style);
        }
    }
    SendMessageW(editWindow, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&originalSelection));
    SendMessageW(editWindow, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(editWindow, nullptr, TRUE);
}

void CapturePageTextFormatting(HWND editWindow, PageText& text, float scale) {
    if (editWindow == nullptr || text.content.empty()) {
        text.styleSpans.clear();
        return;
    }

    HDC dc = GetDC(editWindow);
    const int dpiY = dc != nullptr ? std::max(1, GetDeviceCaps(dc, LOGPIXELSY)) : 96;
    if (dc != nullptr) {
        ReleaseDC(editWindow, dc);
    }
    const float renderScale = std::max(0.1f, scale);
    const PageTextStyle defaultStyle = DefaultPageTextStyle(text);
    const std::vector<PageTextStyleSpan> previousSpans = text.styleSpans;
    const auto previousStyleAt = [&](LONG index) {
        PageTextStyle style = defaultStyle;
        const auto findStyle = [&](LONG position) {
            for (const PageTextStyleSpan& span : previousSpans) {
                const std::size_t start = span.start;
                const std::size_t end = start + span.length;
                if (position >= 0 && static_cast<std::size_t>(position) >= start && static_cast<std::size_t>(position) < end) {
                    return std::optional<PageTextStyle>(span.style);
                }
            }
            return std::optional<PageTextStyle> {};
        };
        if (const auto exact = findStyle(index); exact.has_value()) {
            style = *exact;
        } else if (const auto preceding = findStyle(index - 1); preceding.has_value()) {
            style = *preceding;
        }
        return style;
    };
    SendMessageW(editWindow, WM_SETREDRAW, FALSE, 0);
    CHARRANGE originalSelection {};
    SendMessageW(editWindow, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&originalSelection));

    text.styleSpans.clear();
    std::optional<PageTextStyleSpan> activeSpan;
    for (LONG index = 0; index < static_cast<LONG>(text.content.size()); ++index) {
        CHARRANGE range { index, index + 1 };
        SendMessageW(editWindow, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
        CHARFORMAT2W format {};
        format.cbSize = sizeof(format);
        format.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE;
        SendMessageW(editWindow, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));

        PageTextStyle style {};
        const int pixelHeight = MulDiv(std::max<LONG>(1, format.yHeight), dpiY, 72 * 20);
        style.fontSize = std::max(1.0f, static_cast<float>(pixelHeight) / renderScale);
        style.color = 0xFF000000U
            | (static_cast<std::uint32_t>(GetRValue(format.crTextColor)) << 16U)
            | (static_cast<std::uint32_t>(GetGValue(format.crTextColor)) << 8U)
            | static_cast<std::uint32_t>(GetBValue(format.crTextColor));
        if (_wcsicmp(format.szFaceName, PageTextFontFace(TextFontFamily::Serif)) == 0) {
            style.fontFamily = TextFontFamily::Serif;
        } else if (_wcsicmp(format.szFaceName, PageTextFontFace(TextFontFamily::Monospace)) == 0) {
            style.fontFamily = TextFontFamily::Monospace;
        } else {
            style.fontFamily = TextFontFamily::Sans;
        }
        style.bold = (format.dwEffects & CFE_BOLD) != 0U;
        style.italic = (format.dwEffects & CFE_ITALIC) != 0U;
        style.underline = (format.dwEffects & CFE_UNDERLINE) != 0U;
        const PageTextStyle previousStyle = previousStyleAt(index);
        const float fontRoundingTolerance = std::max(0.05f, 0.6f / renderScale);
        if (std::abs(style.fontSize - previousStyle.fontSize) <= fontRoundingTolerance) {
            style.fontSize = previousStyle.fontSize;
        }

        if (PageTextStyleEqual(style, defaultStyle)) {
            if (activeSpan.has_value()) {
                text.styleSpans.push_back(*activeSpan);
                activeSpan.reset();
            }
        } else if (activeSpan.has_value() && PageTextStyleEqual(activeSpan->style, style)) {
            ++activeSpan->length;
        } else {
            if (activeSpan.has_value()) {
                text.styleSpans.push_back(*activeSpan);
            }
            activeSpan = PageTextStyleSpan {
                static_cast<std::uint32_t>(index),
                1U,
                style,
            };
        }
    }
    if (activeSpan.has_value()) {
        text.styleSpans.push_back(*activeSpan);
    }
    SendMessageW(editWindow, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&originalSelection));
    SendMessageW(editWindow, WM_SETREDRAW, TRUE, 0);
}

UINT DrawTextAlignmentFlags(TextAlignment alignment) {
    switch (alignment) {
    case TextAlignment::Center:
        return DT_CENTER;
    case TextAlignment::Right:
        return DT_RIGHT;
    case TextAlignment::Left:
    default:
        return DT_LEFT;
    }
}

HFONT CreatePageTextFont(const PageText& text, int pixelHeight, DWORD quality) {
    return CreateFontW(
        -pixelHeight,
        0,
        0,
        0,
        PageTextFontWeight(text),
        text.italic ? TRUE : FALSE,
        text.underline ? TRUE : FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS,
        quality,
        DEFAULT_PITCH,
        PageTextFontFace(text.fontFamily));
}

int PageTextFontPixelHeight(const PageText& text, float scale) {
    return std::max(1, static_cast<int>(std::lround(std::max(1.0f, text.fontSize) * std::max(0.1f, scale))));
}

float DefaultPageTextBoxWidth(const Page& page, const PageText& text) {
    const float preferredWidth = std::max(180.0f, text.fontSize * 12.0f);
    if (page.width <= 0.0f) {
        return preferredWidth;
    }
    return std::min(preferredWidth, std::max(120.0f, page.width * 0.6f));
}

float ResolvedPageTextWidth(const PageText& text, float pageWidth) {
    static_cast<void>(pageWidth);
    if (text.width > 0.0f) {
        return std::max(1.0f, text.width);
    }

    const std::wstring measureText = TrimTrailingLayoutWhitespace(text.content);
    const int fontHeight = PageTextFontPixelHeight(text, 1.0f);
    HDC dc = GetDC(nullptr);
    if (dc == nullptr) {
        const float estimatedWidth = static_cast<float>(std::max(
            1,
            static_cast<int>(std::lround(static_cast<float>(measureText.size()) * static_cast<float>(fontHeight) * 0.56f))));
        return estimatedWidth;
    }

    HFONT font = CreatePageTextFont(text, fontHeight);
    HGDIOBJ previousFont = font != nullptr ? SelectObject(dc, font) : nullptr;
    RECT measureRect = MakeRect(0, 0, 32767, 0);
    if (!measureText.empty()) {
        DrawTextW(dc, measureText.c_str(), -1, &measureRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    }
    if (previousFont != nullptr) {
        SelectObject(dc, previousFont);
    }
    if (font != nullptr) {
        DeleteObject(font);
    }
    ReleaseDC(nullptr, dc);
    return std::max(1.0f, static_cast<float>(measureRect.right - measureRect.left));
}

WrappedTextLayout BuildPageTextLayout(const PageText& text, float pageWidth) {
    WrappedTextLayout layout {};
    layout.wrapWidth = std::max(1, static_cast<int>(std::ceil(ResolvedPageTextWidth(text, pageWidth) * kTextLayoutScale)));
    layout.lineHeight = PageTextFontPixelHeight(text, kTextLayoutScale);
    for (const PageTextStyleSpan& span : text.styleSpans) {
        layout.lineHeight = std::max(
            layout.lineHeight,
            static_cast<int>(std::lround(std::max(1.0f, span.style.fontSize) * kTextLayoutScale)));
    }

    if (HWND layoutWindow = RichTextLayoutWindow(); layoutWindow != nullptr) {
        HFONT font = CreatePageTextFont(text, layout.lineHeight);
        SendMessageW(layoutWindow, WM_SETREDRAW, FALSE, 0);
        SendMessageW(layoutWindow, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        if (font != nullptr) {
            SendMessageW(layoutWindow, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        }
        SetWindowPos(layoutWindow, nullptr, 0, 0, layout.wrapWidth, 32767, SWP_NOACTIVATE | SWP_NOZORDER);
        RECT formatRect { 0, 0, layout.wrapWidth, 32767 };
        SendMessageW(layoutWindow, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&formatRect));
        SetWindowTextW(layoutWindow, text.content.c_str());
        ApplyPageTextFormatting(layoutWindow, text, kTextLayoutScale);

        HDC dc = GetDC(layoutWindow);
        if (dc != nullptr) {
            HGDIOBJ previousFont = font != nullptr ? SelectObject(dc, font) : nullptr;
            TEXTMETRICW textMetrics {};
            if (GetTextMetricsW(dc, &textMetrics) != 0) {
                layout.lineHeight = std::max(layout.lineHeight, static_cast<int>(textMetrics.tmHeight));
            }
            if (previousFont != nullptr) {
                SelectObject(dc, previousFont);
            }
            ReleaseDC(layoutWindow, dc);
        }

        const int textLength = static_cast<int>(text.content.size());
        const int lineCount = std::max(1, static_cast<int>(SendMessageW(layoutWindow, EM_GETLINECOUNT, 0, 0)));
        layout.lines.reserve(static_cast<std::size_t>(lineCount));
        for (int lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
            const int start = static_cast<int>(SendMessageW(layoutWindow, EM_LINEINDEX, lineIndex, 0));
            if (start < 0 || start > textLength) {
                layout.lines.emplace_back();
                layout.lineStarts.push_back(static_cast<std::uint32_t>(textLength));
                continue;
            }
            layout.lineStarts.push_back(static_cast<std::uint32_t>(start));
            const int length = std::max(0, static_cast<int>(SendMessageW(layoutWindow, EM_LINELENGTH, start, 0)));
            layout.lines.push_back(TrimTrailingLayoutWhitespace(text.content.substr(
                static_cast<std::size_t>(start),
                static_cast<std::size_t>(std::min(length, textLength - start)))));
        }
        layout.height = std::max(layout.lineHeight, lineCount * layout.lineHeight);
        SetWindowTextW(layoutWindow, L"");
        if (font != nullptr) {
            DeleteObject(font);
        }
        return layout;
    }

    layout.lines.push_back(TrimTrailingLayoutWhitespace(text.content));
    layout.lineStarts.push_back(0U);
    layout.height = std::max(layout.lineHeight, static_cast<int>(layout.lines.size()) * layout.lineHeight);
    return layout;
}

WrappedTextLayout LayoutPageText(const PageText& text, float pageWidth) {
    struct CacheEntry {
        std::uint64_t signature = 0;
        WrappedTextLayout layout {};
    };
    static std::deque<CacheEntry> cache;
    constexpr std::size_t kMaxCachedLayouts = 128;

    const std::uint64_t signature = TextLayoutSignature(text, pageWidth);
    const auto existing = std::find_if(cache.begin(), cache.end(), [&](const CacheEntry& entry) {
        return entry.signature == signature;
    });
    if (existing != cache.end()) {
        CacheEntry entry = std::move(*existing);
        cache.erase(existing);
        cache.push_back(entry);
        return entry.layout;
    }

    WrappedTextLayout layout = BuildPageTextLayout(text, pageWidth);
    cache.push_back({ signature, layout });
    if (cache.size() > kMaxCachedLayouts) {
        cache.pop_front();
    }
    return layout;
}

TextPixelMetrics MeasurePageTextPixels(const PageText& text, float pageWidth, float scale) {
    const WrappedTextLayout layout = LayoutPageText(text, pageWidth);
    const int fontHeight = PageTextFontPixelHeight(text, scale);
    TextPixelMetrics metrics {
        std::max(1, static_cast<int>(std::ceil(ResolvedPageTextWidth(text, pageWidth) * std::max(0.1f, scale)))),
        fontHeight,
    };

    HDC dc = GetDC(nullptr);
    if (dc != nullptr) {
        HFONT font = CreatePageTextFont(text, fontHeight);
        HGDIOBJ previousFont = font != nullptr ? SelectObject(dc, font) : nullptr;
        TEXTMETRICW textMetrics {};
        if (GetTextMetricsW(dc, &textMetrics) != 0) {
            metrics.height = std::max(metrics.height, static_cast<int>(textMetrics.tmHeight));
        }
        if (previousFont != nullptr) {
            SelectObject(dc, previousFont);
        }
        if (font != nullptr) {
            DeleteObject(font);
        }
        ReleaseDC(nullptr, dc);
    }

    metrics.height = std::max(
        metrics.height * std::max(1, static_cast<int>(layout.lines.size())),
        std::max(
            static_cast<int>(std::ceil((static_cast<float>(layout.height) / kTextLayoutScale) * std::max(0.1f, scale))),
            static_cast<int>(std::ceil(text.height * std::max(0.1f, scale)))));
    return metrics;
}

bool DrawPageText(HDC dc, const PageText& text, float pageWidth, float scale, int left, int top, float opacity) {
    HWND layoutWindow = RichTextLayoutWindow();
    if (layoutWindow == nullptr || dc == nullptr || text.content.empty()) {
        return false;
    }

    const float renderScale = std::max(0.1f, scale);
    const int width = std::max(1, static_cast<int>(std::ceil(ResolvedPageTextWidth(text, pageWidth) * renderScale)));
    const int fontHeight = PageTextFontPixelHeight(text, renderScale);
    const TextPixelMetrics metrics = MeasurePageTextPixels(text, pageWidth, renderScale);
    HFONT font = CreatePageTextFont(text, fontHeight);
    SendMessageW(layoutWindow, WM_SETREDRAW, FALSE, 0);
    SendMessageW(layoutWindow, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    if (font != nullptr) {
        SendMessageW(layoutWindow, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    }
    SetWindowPos(layoutWindow, nullptr, 0, 0, width, 32767, SWP_NOACTIVATE | SWP_NOZORDER);
    RECT layoutRect { 0, 0, width, 32767 };
    SendMessageW(layoutWindow, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&layoutRect));
    SetWindowTextW(layoutWindow, text.content.c_str());
    ApplyPageTextFormatting(layoutWindow, text, renderScale, 1.0f);

    PARAFORMAT paragraphFormat {};
    paragraphFormat.cbSize = sizeof(paragraphFormat);
    paragraphFormat.dwMask = PFM_ALIGNMENT;
    switch (text.alignment) {
    case TextAlignment::Center:
        paragraphFormat.wAlignment = PFA_CENTER;
        break;
    case TextAlignment::Right:
        paragraphFormat.wAlignment = PFA_RIGHT;
        break;
    case TextAlignment::Left:
    default:
        paragraphFormat.wAlignment = PFA_LEFT;
        break;
    }
    SendMessageW(layoutWindow, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&paragraphFormat));

    const int dpiX = std::max(1, GetDeviceCaps(dc, LOGPIXELSX));
    const int dpiY = std::max(1, GetDeviceCaps(dc, LOGPIXELSY));
    const auto toTwipsX = [dpiX](int pixels) { return MulDiv(pixels, 1440, dpiX); };
    const auto toTwipsY = [dpiY](int pixels) { return MulDiv(pixels, 1440, dpiY); };
    const int height = std::max(1, metrics.height);
    HDC blackDc = CreateCompatibleDC(dc);
    HDC whiteDc = CreateCompatibleDC(dc);
    void* blackBits = nullptr;
    void* whiteBits = nullptr;
    HBITMAP blackBitmap = blackDc != nullptr ? CreateTextRenderBitmap(blackDc, width, height, &blackBits) : nullptr;
    HBITMAP whiteBitmap = whiteDc != nullptr ? CreateTextRenderBitmap(whiteDc, width, height, &whiteBits) : nullptr;
    if (blackDc == nullptr || whiteDc == nullptr || blackBitmap == nullptr || whiteBitmap == nullptr
        || blackBits == nullptr || whiteBits == nullptr) {
        if (blackBitmap != nullptr) DeleteObject(blackBitmap);
        if (whiteBitmap != nullptr) DeleteObject(whiteBitmap);
        if (blackDc != nullptr) DeleteDC(blackDc);
        if (whiteDc != nullptr) DeleteDC(whiteDc);
        SetWindowTextW(layoutWindow, L"");
        if (font != nullptr) DeleteObject(font);
        return false;
    }

    HGDIOBJ previousBlackBitmap = SelectObject(blackDc, blackBitmap);
    HGDIOBJ previousWhiteBitmap = SelectObject(whiteDc, whiteBitmap);
    RECT renderRect { 0, 0, width, height };
    FillRect(blackDc, &renderRect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    FillRect(whiteDc, &renderRect, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

    FORMATRANGE formatRange {};
    formatRange.hdcTarget = dc;
    formatRange.rc.left = 0;
    formatRange.rc.top = 0;
    formatRange.rc.right = toTwipsX(width);
    formatRange.rc.bottom = toTwipsY(height);
    formatRange.rcPage.left = 0;
    formatRange.rcPage.top = 0;
    formatRange.rcPage.right = formatRange.rc.right;
    formatRange.rcPage.bottom = formatRange.rc.bottom;
    formatRange.chrg.cpMin = 0;
    formatRange.chrg.cpMax = -1;

    SendMessageW(layoutWindow, EM_SETBKGNDCOLOR, FALSE, RGB(0, 0, 0));
    formatRange.hdc = blackDc;
    SendMessageW(layoutWindow, EM_FORMATRANGE, TRUE, reinterpret_cast<LPARAM>(&formatRange));
    SendMessageW(layoutWindow, EM_FORMATRANGE, FALSE, 0);
    SendMessageW(layoutWindow, EM_SETBKGNDCOLOR, FALSE, RGB(255, 255, 255));
    formatRange.hdc = whiteDc;
    SendMessageW(layoutWindow, EM_FORMATRANGE, TRUE, reinterpret_cast<LPARAM>(&formatRange));
    SendMessageW(layoutWindow, EM_FORMATRANGE, FALSE, 0);
    SendMessageW(layoutWindow, EM_SETBKGNDCOLOR, TRUE, 0);

    auto* blackPixels = static_cast<std::uint8_t*>(blackBits);
    const auto* whitePixels = static_cast<const std::uint8_t*>(whiteBits);
    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const float clampedOpacity = std::clamp(opacity, 0.0f, 1.0f);
    for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex) {
        std::uint8_t* blackPixel = blackPixels + (pixelIndex * 4U);
        const std::uint8_t* whitePixel = whitePixels + (pixelIndex * 4U);
        const int blueDelta = static_cast<int>(whitePixel[0]) - static_cast<int>(blackPixel[0]);
        const int greenDelta = static_cast<int>(whitePixel[1]) - static_cast<int>(blackPixel[1]);
        const int redDelta = static_cast<int>(whitePixel[2]) - static_cast<int>(blackPixel[2]);
        const int coverage = std::clamp(255 - ((blueDelta + greenDelta + redDelta) / 3), 0, 255);
        const int alpha = static_cast<int>(std::lround(static_cast<float>(coverage) * clampedOpacity));
        if (alpha == 0) {
            blackPixel[0] = 0;
            blackPixel[1] = 0;
            blackPixel[2] = 0;
            blackPixel[3] = 0;
            continue;
        }
        blackPixel[0] = static_cast<std::uint8_t>(std::clamp(
            static_cast<int>(std::lround(static_cast<float>(blackPixel[0]) * clampedOpacity)), 0, alpha));
        blackPixel[1] = static_cast<std::uint8_t>(std::clamp(
            static_cast<int>(std::lround(static_cast<float>(blackPixel[1]) * clampedOpacity)), 0, alpha));
        blackPixel[2] = static_cast<std::uint8_t>(std::clamp(
            static_cast<int>(std::lround(static_cast<float>(blackPixel[2]) * clampedOpacity)), 0, alpha));
        blackPixel[3] = static_cast<std::uint8_t>(alpha);
    }

    BLENDFUNCTION blendFunction { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    const BOOL blended = AlphaBlend(dc, left, top, width, height, blackDc, 0, 0, width, height, blendFunction);

    SelectObject(blackDc, previousBlackBitmap);
    SelectObject(whiteDc, previousWhiteBitmap);
    DeleteObject(blackBitmap);
    DeleteObject(whiteBitmap);
    DeleteDC(blackDc);
    DeleteDC(whiteDc);
    SetWindowTextW(layoutWindow, L"");
    if (font != nullptr) {
        DeleteObject(font);
    }
    return blended != FALSE;
}

PageTextBounds MeasurePageTextBounds(const PageText& text, float pageWidth) {
    const TextPixelMetrics metrics = MeasurePageTextPixels(text, pageWidth, 1.0f);
    return {
        text.x,
        text.y,
        text.x + ResolvedPageTextWidth(text, pageWidth),
        text.y + std::max(std::max(1.0f, static_cast<float>(metrics.height)), text.height),
    };
}

std::optional<RECT> PageTextBoundsRect(const PageText& text, float pageWidth, float scale, int extraPadding) {
    if (scale <= 0.0f) {
        return std::nullopt;
    }

    const PageTextBounds bounds = MeasurePageTextBounds(text, pageWidth);
    const float intrinsicPadding = static_cast<float>(PageTextRenderPaddingPixels(text, scale) + extraPadding);
    return MakeRectFromFloatBounds(
        (bounds.left * scale) - intrinsicPadding,
        (bounds.top * scale) - intrinsicPadding,
        (bounds.right * scale) + intrinsicPadding + 1.0f,
        (bounds.bottom * scale) + intrinsicPadding + 1.0f);
}

std::optional<RECT> PageTextCachedBoundsRect(const PageText& text, float boundsWidth, float boundsHeight, float scale, int extraPadding) {
    if (scale <= 0.0f) {
        return std::nullopt;
    }

    const float intrinsicPadding = static_cast<float>(PageTextRenderPaddingPixels(text, scale) + extraPadding);
    return MakeRectFromFloatBounds(
        (text.x * scale) - intrinsicPadding,
        (text.y * scale) - intrinsicPadding,
        ((text.x + std::max(1.0f, boundsWidth)) * scale) + intrinsicPadding + 1.0f,
        ((text.y + std::max(1.0f, boundsHeight)) * scale) + intrinsicPadding + 1.0f);
}

bool TextHitsPoint(const PageText& text, float pageWidth, Vec2 point) {
    const PageTextBounds bounds = MeasurePageTextBounds(text, pageWidth);
    return point.x >= bounds.left && point.x <= bounds.right
        && point.y >= bounds.top && point.y <= bounds.bottom;
}

bool TextIntersectsRectSelection(const PageText& text, float pageWidth, Vec2 anchor, Vec2 current) {
    const PageTextBounds bounds = MeasurePageTextBounds(text, pageWidth);
    const float left = std::min(anchor.x, current.x);
    const float right = std::max(anchor.x, current.x);
    const float top = std::min(anchor.y, current.y);
    const float bottom = std::max(anchor.y, current.y);
    return !(bounds.right < left || bounds.left > right || bounds.bottom < top || bounds.top > bottom);
}

bool TextIntersectsPolygonSelection(const PageText& text, float pageWidth, const std::vector<Vec2>& polygon) {
    if (polygon.size() < 3U) {
        return false;
    }

    const PageTextBounds bounds = MeasurePageTextBounds(text, pageWidth);
    const std::array<Vec2, 4> corners {{
        { bounds.left, bounds.top },
        { bounds.right, bounds.top },
        { bounds.right, bounds.bottom },
        { bounds.left, bounds.bottom },
    }};

    for (const Vec2& corner : corners) {
        if (PointInPolygon(corner, polygon)) {
            return true;
        }
    }

    for (const Vec2& point : polygon) {
        if (TextHitsPoint(text, pageWidth, point)) {
            return true;
        }
    }

    for (std::size_t edgeIndex = 0; edgeIndex < polygon.size(); ++edgeIndex) {
        const Vec2 polyA = polygon[edgeIndex];
        const Vec2 polyB = polygon[(edgeIndex + 1U) % polygon.size()];
        for (std::size_t rectEdgeIndex = 0; rectEdgeIndex < corners.size(); ++rectEdgeIndex) {
            if (SegmentsIntersect(polyA, polyB, corners[rectEdgeIndex], corners[(rectEdgeIndex + 1U) % corners.size()])) {
                return true;
            }
        }
    }

    return false;
}

}  // namespace cnote::text
