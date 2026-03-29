#include "text/TextGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>

namespace cnote::text {

namespace {

constexpr float kTextLayoutScale = 8.0f;

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

}  // namespace

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

WrappedTextLayout LayoutPageText(const PageText& text, float pageWidth) {
    WrappedTextLayout layout {};
    layout.wrapWidth = std::max(1, static_cast<int>(std::ceil(ResolvedPageTextWidth(text, pageWidth) * kTextLayoutScale)));
    layout.lineHeight = PageTextFontPixelHeight(text, kTextLayoutScale);

    HDC dc = GetDC(nullptr);
    if (dc == nullptr) {
        layout.lines.push_back(TrimTrailingLayoutWhitespace(text.content));
        if (layout.lines.back().empty()) {
            layout.lines.back().clear();
        }
        layout.height = std::max(layout.lineHeight, static_cast<int>(layout.lines.size()) * layout.lineHeight);
        return layout;
    }

    HFONT font = CreatePageTextFont(text, layout.lineHeight);
    HGDIOBJ previousFont = font != nullptr ? SelectObject(dc, font) : nullptr;

    TEXTMETRICW textMetrics {};
    if (GetTextMetricsW(dc, &textMetrics) != 0) {
        layout.lineHeight = std::max(layout.lineHeight, static_cast<int>(textMetrics.tmHeight));
    }

    auto appendParagraph = [&](const std::wstring& paragraph) {
        if (paragraph.empty()) {
            layout.lines.push_back(L"");
            return;
        }

        std::size_t index = 0;
        while (index < paragraph.size()) {
            const int remaining = static_cast<int>(paragraph.size() - index);
            int fit = 0;
            SIZE fitSize {};
            if (GetTextExtentExPointW(dc, paragraph.c_str() + index, remaining, layout.wrapWidth, &fit, nullptr, &fitSize) == 0) {
                fit = 0;
            }
            fit = std::clamp(fit, 1, remaining);

            int take = fit;
            bool brokeAtWhitespace = false;
            if (fit < remaining) {
                int whitespaceTake = fit;
                while (whitespaceTake > 0 && !std::iswspace(paragraph[index + whitespaceTake - 1])) {
                    --whitespaceTake;
                }
                if (whitespaceTake > 0) {
                    take = whitespaceTake;
                    brokeAtWhitespace = true;
                }
            }

            take = std::max(1, take);
            std::wstring line = paragraph.substr(index, static_cast<std::size_t>(take));
            layout.lines.push_back(TrimTrailingLayoutWhitespace(std::move(line)));
            index += static_cast<std::size_t>(take);

            if (brokeAtWhitespace) {
                while (index < paragraph.size() && (paragraph[index] == L' ' || paragraph[index] == L'\t')) {
                    ++index;
                }
            }
        }
    };

    std::wstring paragraph;
    for (std::size_t index = 0; index < text.content.size(); ++index) {
        const wchar_t ch = text.content[index];
        if (ch == L'\r' || ch == L'\n') {
            appendParagraph(paragraph);
            paragraph.clear();
            if (ch == L'\r' && (index + 1U) < text.content.size() && text.content[index + 1U] == L'\n') {
                ++index;
            }
        } else {
            paragraph.push_back(ch);
        }
    }
    appendParagraph(paragraph);

    if (previousFont != nullptr) {
        SelectObject(dc, previousFont);
    }
    if (font != nullptr) {
        DeleteObject(font);
    }
    ReleaseDC(nullptr, dc);

    if (layout.lines.empty()) {
        layout.lines.push_back(L"");
    }
    layout.height = std::max(layout.lineHeight, static_cast<int>(layout.lines.size()) * layout.lineHeight);
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
