#include "App.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include <commdlg.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dcompanimation.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <objidl.h>
#include <gdiplus.h>
#include <mmsystem.h>
#include <windowsx.h>

#include "DpiUtil.h"
#include "resource.h"
#include "NewPageSettingsWindow.h"
#include "SettingsWindow.h"
#include "ShortcutConfig.h"
#include "ui/UiUtil.h"

namespace cnote {

namespace ui = cnote::ui;

namespace {

using ui::FillRoundedRect;
using ui::StrokeRoundedRect;

template <typename T>
void ReleaseCom(T*& instance) {
    if (instance != nullptr) {
        instance->Release();
        instance = nullptr;
    }
}

void HashToolbarBytes(std::uint64_t& hash, const void* data, std::size_t size) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= static_cast<std::uint64_t>(bytes[index]);
        hash *= kFnvPrime;
    }
}

template <typename T>
void HashToolbarValue(std::uint64_t& hash, const T& value) {
    HashToolbarBytes(hash, &value, sizeof(value));
}

void HashToolbarString(std::uint64_t& hash, const std::wstring& value) {
    const std::size_t size = value.size();
    HashToolbarValue(hash, size);
    if (!value.empty()) {
        HashToolbarBytes(hash, value.data(), value.size() * sizeof(wchar_t));
    }
}

constexpr wchar_t kWindowClassName[] = L"CnoteWindow";
constexpr wchar_t kWindowTitle[] = L"cnote";
constexpr int kToolbarPadding = 18;
constexpr int kIconButtonSize = 30;
constexpr int kColorSwatchSize = 22;
constexpr int kColorSwatchGap = 10;
constexpr int kSelectionHandleHalfDip = 4;
constexpr float kSelectionHandleHitRadiusDip = 11.0f;
constexpr float kSelectionRotateOffsetDip = 18.0f;
constexpr int kPaletteWheelShadowInsetDip = 6;
constexpr int kPaletteWheelIconHalfDip = 20;
constexpr int kPaletteWheelHoverOffsetDip = 8;
constexpr int kPaletteWheelHoverGrowDip = 8;
constexpr float kPaletteWheelSelectedStrokeDip = 3.0f;
constexpr float kPaletteWheelStrokeDip = 1.2f;
constexpr float kPaletteWheelBorderDip = 1.5f;
constexpr int kQuickSizePreviewMinDip = 4;
constexpr int kQuickSizeThumbHalfDip = 6;
constexpr int kQuickSizeValuePaddingDip = 5;
constexpr int kQuickSizeCaretInsetDip = 6;
constexpr BYTE kEraserPreviewAlpha = 255;
constexpr float kEraserMinRadiusPixels = 3.5f;
constexpr float kEraserOutlinePixels = 0.75f;
constexpr float kMinDrawCursorDiameterPixels = 4.0f;
constexpr float kMaxDrawCursorDiameterPixels = 24.0f;
constexpr float kPendingEraseObjectOpacity = 0.5f;
constexpr int kQuickWheelBadgeInsetRightDip = 3;
constexpr int kQuickWheelBadgeTopDip = 4;
constexpr int kQuickWheelBadgeSizeDip = 6;
constexpr unsigned int kCommandFileNew = 1001;
constexpr unsigned int kCommandFileOpen = 1002;
constexpr unsigned int kCommandFileSaveCnote = 1003;
constexpr unsigned int kCommandFileExportPdf = 1004;
constexpr unsigned int kCommandFileExit = 1005;
constexpr unsigned int kCommandEditBrushSettings = 1103;
constexpr unsigned int kCommandSelectLasso = 1111;
constexpr unsigned int kCommandToolToggleQuickWheel = 1301;
constexpr unsigned int kCommandRecentDocumentBase = 1400;

struct ToolbarLayout {
    RECT titleAreaRect {};
    RECT minimizeRect {};
    RECT maximizeRect {};
    RECT closeRect {};
    RECT undoRect {};
    RECT redoRect {};
    RECT selectToolRect {};
    RECT textToolRect {};
    RECT moveToolRect {};
    RECT drawToolRect {};
    RECT panToolRect {};
    RECT eraseToolRect {};
    RECT currentColorRect {};
};

struct ToolbarTitleLayout {
    RECT fileTitleRect {};
    RECT titleTextRect {};
    RECT zoomRect {};
    RECT zoomTextRect {};
    int dividerX = 0;
};

struct SidebarHeaderLayout {
    RECT headerRect {};
    RECT previousPageRect {};
    RECT pageLabelRect {};
    RECT nextPageRect {};
    RECT addPageRect {};
    RECT newPageSettingsRect {};
    RECT contentRect {};
};

struct SidebarMetrics {
    int width = 0;
    int itemInset = 0;
    int contentPadding = 0;
    int itemGap = 0;
    int labelHeight = 0;
    int minThumbHeight = 0;
    int placeholderInset = 0;
    int placeholderLineGap = 0;
};

struct QuickSizePopupLayout {
    RECT panelRect {};
    RECT previewRect {};
    RECT sliderRect {};
    RECT valueRect {};
};

struct TextFormatPopupLayout {
    RECT panelRect {};
    RECT alignCycleRect {};
    RECT boldRect {};
    RECT italicRect {};
    RECT underlineRect {};
    RECT textSizeInputRect {};
    RECT fontDropdownRect {};
    RECT fontMenuRect {};
    std::array<RECT, 3> fontOptionRects {};
};

struct PaletteMenuLayout {
    RECT panelRect {};
    std::vector<RECT> swatchRects;
};

struct PaletteWheelLayout {
    POINT center {};
    int innerRadius = 0;
    int outerRadius = 0;
    int hitOuterRadius = 0;
};

bool SegmentsIntersect(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2);
bool PointInPolygon(Vec2 point, const std::vector<Vec2>& polygon);

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

int SelectionHandleHalfSize(UINT dpi) {
    return std::max(1, ScaleForDpi(kSelectionHandleHalfDip, dpi));
}

float SelectionHandleHitRadiusPixels(UINT dpi) {
    return std::max(
        static_cast<float>(SelectionHandleHalfSize(dpi) + ScaleForDpi(2, dpi)),
        ScaleForDpi(kSelectionHandleHitRadiusDip, dpi));
}

float SelectionRotateOffsetPageUnits(float pageScale, UINT dpi) {
    return ScaleForDpi(kSelectionRotateOffsetDip, dpi) / std::max(0.1f, pageScale);
}

int PaletteWheelShadowInset(UINT dpi) {
    return ScaleForDpi(kPaletteWheelShadowInsetDip, dpi);
}

int PaletteWheelIconHalfSize(UINT dpi) {
    return std::max(1, ScaleForDpi(kPaletteWheelIconHalfDip, dpi));
}

int PaletteWheelHoverOffset(UINT dpi) {
    return ScaleForDpi(kPaletteWheelHoverOffsetDip, dpi);
}

int PaletteWheelHoverGrow(UINT dpi) {
    return ScaleForDpi(kPaletteWheelHoverGrowDip, dpi);
}

float PaletteWheelStrokeWidth(float baseDip, UINT dpi) {
    return std::max(1.0f, ScaleForDpi(baseDip, dpi));
}

int QuickSizeThumbHalfSize(UINT dpi) {
    return std::max(1, ScaleForDpi(kQuickSizeThumbHalfDip, dpi));
}

bool HasRect(const RECT& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool RectsOverlap(const RECT& a, const RECT& b) {
    RECT overlap {};
    return IntersectRect(&overlap, &a, &b) != 0;
}

bool Vec2Equal(const Vec2& a, const Vec2& b) {
    return a.x == b.x && a.y == b.y;
}

bool StrokeStyleEqual(const StrokeStyle& a, const StrokeStyle& b) {
    return a.color == b.color && a.width == b.width && a.dotted == b.dotted;
}

bool StrokeEqual(const Stroke& a, const Stroke& b) {
    if (!StrokeStyleEqual(a.style, b.style) || a.smoothing != b.smoothing || a.points.size() != b.points.size()) {
        return false;
    }

    for (std::size_t index = 0; index < a.points.size(); ++index) {
        if (!Vec2Equal(a.points[index], b.points[index])) {
            return false;
        }
    }
    return true;
}

bool PageImageEqual(const PageImage& a, const PageImage& b) {
    return a.x == b.x
        && a.y == b.y
        && a.width == b.width
        && a.height == b.height
        && a.rotationDegrees == b.rotationDegrees
        && a.pixelWidth == b.pixelWidth
        && a.pixelHeight == b.pixelHeight
        && a.stride == b.stride
        && a.pixels == b.pixels;
}

bool PageTextEqual(const PageText& a, const PageText& b) {
    return a.x == b.x
        && a.y == b.y
        && a.width == b.width
        && a.height == b.height
        && a.fontSize == b.fontSize
        && a.color == b.color
        && a.alignment == b.alignment
        && a.fontFamily == b.fontFamily
        && a.bold == b.bold
        && a.italic == b.italic
        && a.underline == b.underline
        && a.content == b.content;
}

void HashPageSurfaceBytes(std::uint64_t& hash, const void* data, std::size_t size) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= static_cast<std::uint64_t>(bytes[index]);
        hash *= kFnvPrime;
    }
}

template <typename T>
void HashPageSurfaceValue(std::uint64_t& hash, const T& value) {
    HashPageSurfaceBytes(hash, &value, sizeof(value));
}

std::uint64_t PageTextSurfaceSignature(const Page& page) {
    std::uint64_t hash = 14695981039346656037ULL;
    HashPageSurfaceValue(hash, page.texts.size());
    for (const PageText& text : page.texts) {
        HashPageSurfaceValue(hash, text.x);
        HashPageSurfaceValue(hash, text.y);
        HashPageSurfaceValue(hash, text.width);
        HashPageSurfaceValue(hash, text.height);
        HashPageSurfaceValue(hash, text.fontSize);
        HashPageSurfaceValue(hash, text.color);
        HashPageSurfaceValue(hash, text.alignment);
        HashPageSurfaceValue(hash, text.fontFamily);
        HashPageSurfaceValue(hash, text.bold);
        HashPageSurfaceValue(hash, text.italic);
        HashPageSurfaceValue(hash, text.underline);
        HashPageSurfaceValue(hash, text.content.size());
        if (!text.content.empty()) {
            HashPageSurfaceBytes(hash, text.content.data(), text.content.size() * sizeof(wchar_t));
        }
    }
    return hash;
}

bool RasterImageEqual(const RasterImage& a, const RasterImage& b) {
    return a.width == b.width
        && a.height == b.height
        && a.stride == b.stride
        && a.scale == b.scale
        && a.pixels == b.pixels;
}

bool PageEqual(const Page& a, const Page& b) {
    if (a.width != b.width
        || a.height != b.height
        || a.sourcePdfSourceIndex != b.sourcePdfSourceIndex
        || a.sourcePdfPageIndex != b.sourcePdfPageIndex
        || !RasterImageEqual(a.raster, b.raster)
        || a.images.size() != b.images.size()
        || a.texts.size() != b.texts.size()
        || a.strokes.size() != b.strokes.size()) {
        return false;
    }

    for (std::size_t index = 0; index < a.images.size(); ++index) {
        if (!PageImageEqual(a.images[index], b.images[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < a.texts.size(); ++index) {
        if (!PageTextEqual(a.texts[index], b.texts[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < a.strokes.size(); ++index) {
        if (!StrokeEqual(a.strokes[index], b.strokes[index])) {
            return false;
        }
    }
    return true;
}

struct PageTextBounds {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

struct TextPixelMetrics {
    int width = 0;
    int height = 0;
};

struct WrappedTextLayout {
    std::vector<std::wstring> lines;
    int wrapWidth = 0;
    int lineHeight = 0;
    int height = 0;
};

constexpr float kTextLayoutScale = 8.0f;

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

HFONT CreatePageTextFont(const PageText& text, int pixelHeight, DWORD quality = CLEARTYPE_QUALITY) {
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

int PageTextRenderPaddingPixels(const PageText& text, float scale) {
    return std::max(2, static_cast<int>(std::ceil(std::max(1.0f, text.fontSize) * std::max(0.1f, scale) * 0.12f)));
}

std::wstring TrimTrailingLayoutWhitespace(std::wstring value) {
    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) {
        value.pop_back();
    }
    return value;
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
        const float estimatedWidth = static_cast<float>(std::max(1, static_cast<int>(std::lround(static_cast<float>(measureText.size()) * static_cast<float>(fontHeight) * 0.56f))));
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
    const float measuredWidth = std::max(1.0f, static_cast<float>(measureRect.right - measureRect.left));
    return measuredWidth;
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
            if (GetTextExtentExPointW(
                    dc,
                    paragraph.c_str() + index,
                    remaining,
                    layout.wrapWidth,
                    &fit,
                    nullptr,
                    &fitSize) == 0) {
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

std::optional<RECT> PageTextBoundsRect(const PageText& text, float pageWidth, float scale, int extraPadding = 0) {
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

std::optional<RECT> PageTextCachedBoundsRect(const PageText& text, float boundsWidth, float boundsHeight, float scale, int extraPadding = 0) {
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

std::optional<RECT> StrokeBoundsRect(const Stroke& stroke, float scale, int extraPadding = 0) {
    if (stroke.points.empty() || scale <= 0.0f) {
        return std::nullopt;
    }

    float minX = stroke.points.front().x;
    float minY = stroke.points.front().y;
    float maxX = stroke.points.front().x;
    float maxY = stroke.points.front().y;
    for (const Vec2& point : stroke.points) {
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
        maxX = std::max(maxX, point.x);
        maxY = std::max(maxY, point.y);
    }

    const float penWidth = std::max(1.25f, stroke.style.width * scale / 2.15f);
    const float padding = static_cast<float>(static_cast<int>(std::ceil(penWidth)) + extraPadding);
    return MakeRectFromFloatBounds(
        (minX * scale) - padding,
        (minY * scale) - padding,
        (maxX * scale) + padding + 1.0f,
        (maxY * scale) + padding + 1.0f);
}

RECT UnionRectangles(RECT a, RECT b) {
    if (!HasRect(a)) {
        return b;
    }
    if (!HasRect(b)) {
        return a;
    }

    return MakeRect(
        std::min(a.left, b.left),
        std::min(a.top, b.top),
        std::max(a.right, b.right),
        std::max(a.bottom, b.bottom));
}

RECT InflateRectCopy(RECT rect, int amount) {
    rect.left -= amount;
    rect.top -= amount;
    rect.right += amount;
    rect.bottom += amount;
    return rect;
}

int WindowFrameThicknessX(UINT dpi) {
    return SystemMetricForDpi(SM_CXFRAME, dpi) + SystemMetricForDpi(SM_CXPADDEDBORDER, dpi);
}

int CaptionButtonReserve(UINT dpi) {
    return ScaleForDpi((35 * 3) + 12, dpi);
}

constexpr int kBaseToolbarHeight = 34;
constexpr int kMaximizedToolbarExtraHeight = 5;
constexpr int kTitleBarButtonHeight = 34;
constexpr DWORD kDwmWindowCornerPreferenceAttribute = 33;
constexpr DWORD kDwmWindowCornerRound = 2;

int ToolbarHeight(bool maximized, UINT dpi) {
    return ScaleForDpi(kBaseToolbarHeight + (maximized ? kMaximizedToolbarExtraHeight : 0), dpi);
}

int ToolbarTopInset(bool maximized, UINT dpi) {
    return maximized ? ScaleForDpi(kMaximizedToolbarExtraHeight, dpi) : 0;
}

HBITMAP CreateTopDown32BitDibSection(HDC dc, int width, int height, void** bits) {
    if (bits == nullptr || width <= 0 || height <= 0) {
        return nullptr;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS, bits, nullptr, 0);
}

SidebarMetrics BuildSidebarMetrics(UINT dpi) {
    SidebarMetrics metrics {};
    metrics.width = ScaleForDpi(176, dpi);
    metrics.itemInset = ScaleForDpi(12, dpi);
    metrics.contentPadding = ScaleForDpi(12, dpi);
    metrics.itemGap = ScaleForDpi(10, dpi);
    metrics.labelHeight = ScaleForDpi(22, dpi);
    metrics.minThumbHeight = ScaleForDpi(56, dpi);
    metrics.placeholderInset = ScaleForDpi(14, dpi);
    metrics.placeholderLineGap = ScaleForDpi(10, dpi);
    return metrics;
}

HCURSOR CreateBrushCursor(int brushDiameter, COLORREF fillColor) {
    const int cursorSize = std::max(32, brushDiameter + 2);
    const int hotspot = cursorSize / 2;
    const float radius = static_cast<float>(brushDiameter) * 0.5f;
    const float center = static_cast<float>(hotspot) + 0.5f;
    constexpr std::uint32_t kTransparent = 0x00000000;

    BITMAPV5HEADER bitmapHeader {};
    bitmapHeader.bV5Size = sizeof(bitmapHeader);
    bitmapHeader.bV5Width = cursorSize;
    bitmapHeader.bV5Height = -cursorSize;
    bitmapHeader.bV5Planes = 1;
    bitmapHeader.bV5BitCount = 32;
    bitmapHeader.bV5Compression = BI_BITFIELDS;
    bitmapHeader.bV5RedMask = 0x00FF0000;
    bitmapHeader.bV5GreenMask = 0x0000FF00;
    bitmapHeader.bV5BlueMask = 0x000000FF;
    bitmapHeader.bV5AlphaMask = 0xFF000000;

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        return nullptr;
    }

    void* colorBits = nullptr;
    HBITMAP colorBitmap = CreateDIBSection(screenDc, reinterpret_cast<BITMAPINFO*>(&bitmapHeader), DIB_RGB_COLORS, &colorBits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);
    if (colorBitmap == nullptr || colorBits == nullptr) {
        if (colorBitmap != nullptr) {
            DeleteObject(colorBitmap);
        }
        return nullptr;
    }

    auto* pixels = static_cast<std::uint32_t*>(colorBits);
    std::fill_n(pixels, cursorSize * cursorSize, kTransparent);
    const auto alphaPixel = [](BYTE alpha, BYTE red, BYTE green, BYTE blue) {
        return (static_cast<std::uint32_t>(alpha) << 24U)
            | (static_cast<std::uint32_t>(red) << 16U)
            | (static_cast<std::uint32_t>(green) << 8U)
            | static_cast<std::uint32_t>(blue);
    };
    const auto edgeCoverage = [](float distance, float edge) {
        return std::clamp(edge + 0.5f - distance, 0.0f, 1.0f);
    };
    const float outlineInnerRadius = std::max(0.0f, radius - kEraserOutlinePixels);
    constexpr int kSampleCount = 3;
    constexpr float kSampleWeight = 1.0f / static_cast<float>(kSampleCount * kSampleCount);
    for (int y = 0; y < cursorSize; ++y) {
        for (int x = 0; x < cursorSize; ++x) {
            float fillCoverage = 0.0f;
            float outlineCoverage = 0.0f;
            for (int sampleY = 0; sampleY < kSampleCount; ++sampleY) {
                for (int sampleX = 0; sampleX < kSampleCount; ++sampleX) {
                    const float dx = (static_cast<float>(x) + ((static_cast<float>(sampleX) + 0.5f) / static_cast<float>(kSampleCount))) - center;
                    const float dy = (static_cast<float>(y) + ((static_cast<float>(sampleY) + 0.5f) / static_cast<float>(kSampleCount))) - center;
                    const float distance = std::sqrt((dx * dx) + (dy * dy));
                    const float outerCoverage = edgeCoverage(distance, radius);
                    if (outerCoverage <= 0.0f) {
                        continue;
                    }

                    const float sampleFillCoverage = std::min(outerCoverage, edgeCoverage(distance, outlineInnerRadius));
                    fillCoverage += sampleFillCoverage * kSampleWeight;
                    outlineCoverage += std::max(0.0f, outerCoverage - sampleFillCoverage) * kSampleWeight;
                }
            }
            const float totalCoverage = std::clamp(fillCoverage + outlineCoverage, 0.0f, 1.0f);
            if (totalCoverage <= 0.0f) {
                continue;
            }

            const float fillContribution = fillCoverage / totalCoverage;
            const BYTE alpha = static_cast<BYTE>(std::lround(totalCoverage * 255.0f));
            const BYTE red = static_cast<BYTE>(std::lround(static_cast<float>(GetRValue(fillColor)) * fillContribution));
            const BYTE green = static_cast<BYTE>(std::lround(static_cast<float>(GetGValue(fillColor)) * fillContribution));
            const BYTE blue = static_cast<BYTE>(std::lround(static_cast<float>(GetBValue(fillColor)) * fillContribution));
            pixels[(y * cursorSize) + x] = alphaPixel(alpha, red, green, blue);
        }
    }

    HBITMAP maskBitmap = CreateBitmap(cursorSize, cursorSize, 1, 1, nullptr);
    if (maskBitmap == nullptr) {
        DeleteObject(colorBitmap);
        return nullptr;
    }

    ICONINFO iconInfo {};
    iconInfo.fIcon = FALSE;
    iconInfo.xHotspot = hotspot;
    iconInfo.yHotspot = hotspot;
    iconInfo.hbmMask = maskBitmap;
    iconInfo.hbmColor = colorBitmap;
    HCURSOR cursor = CreateIconIndirect(&iconInfo);

    DeleteObject(maskBitmap);
    DeleteObject(colorBitmap);
    return cursor;
}

HCURSOR CreateSquareBrushCursor(int brushDiameter, COLORREF fillColor) {
    const int cursorSize = std::max(32, brushDiameter + 2);
    const int hotspot = cursorSize / 2;
    const float halfSize = static_cast<float>(brushDiameter) * 0.5f;
    const float center = static_cast<float>(hotspot) + 0.5f;
    constexpr std::uint32_t kTransparent = 0x00000000;

    BITMAPV5HEADER bitmapHeader {};
    bitmapHeader.bV5Size = sizeof(bitmapHeader);
    bitmapHeader.bV5Width = cursorSize;
    bitmapHeader.bV5Height = -cursorSize;
    bitmapHeader.bV5Planes = 1;
    bitmapHeader.bV5BitCount = 32;
    bitmapHeader.bV5Compression = BI_BITFIELDS;
    bitmapHeader.bV5RedMask = 0x00FF0000;
    bitmapHeader.bV5GreenMask = 0x0000FF00;
    bitmapHeader.bV5BlueMask = 0x000000FF;
    bitmapHeader.bV5AlphaMask = 0xFF000000;

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        return nullptr;
    }

    void* colorBits = nullptr;
    HBITMAP colorBitmap = CreateDIBSection(screenDc, reinterpret_cast<BITMAPINFO*>(&bitmapHeader), DIB_RGB_COLORS, &colorBits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);
    if (colorBitmap == nullptr || colorBits == nullptr) {
        if (colorBitmap != nullptr) {
            DeleteObject(colorBitmap);
        }
        return nullptr;
    }

    auto* pixels = static_cast<std::uint32_t*>(colorBits);
    std::fill_n(pixels, cursorSize * cursorSize, kTransparent);
    const auto alphaPixel = [](BYTE alpha, BYTE red, BYTE green, BYTE blue) {
        return (static_cast<std::uint32_t>(alpha) << 24U)
            | (static_cast<std::uint32_t>(red) << 16U)
            | (static_cast<std::uint32_t>(green) << 8U)
            | static_cast<std::uint32_t>(blue);
    };
    const auto edgeCoverage = [](float distanceToEdge) {
        return std::clamp(distanceToEdge + 0.5f, 0.0f, 1.0f);
    };

    const float innerHalfSize = std::max(0.0f, halfSize - kEraserOutlinePixels);
    constexpr int kSampleCount = 3;
    constexpr float kSampleWeight = 1.0f / static_cast<float>(kSampleCount * kSampleCount);
    for (int y = 0; y < cursorSize; ++y) {
        for (int x = 0; x < cursorSize; ++x) {
            float fillCoverage = 0.0f;
            float outlineCoverage = 0.0f;
            for (int sampleY = 0; sampleY < kSampleCount; ++sampleY) {
                for (int sampleX = 0; sampleX < kSampleCount; ++sampleX) {
                    const float dx = std::abs((static_cast<float>(x) + ((static_cast<float>(sampleX) + 0.5f) / static_cast<float>(kSampleCount))) - center);
                    const float dy = std::abs((static_cast<float>(y) + ((static_cast<float>(sampleY) + 0.5f) / static_cast<float>(kSampleCount))) - center);
                    const float outerDistanceToEdge = halfSize - std::max(dx, dy);
                    const float outerCoverage = edgeCoverage(outerDistanceToEdge);
                    if (outerCoverage <= 0.0f) {
                        continue;
                    }

                    const float innerDistanceToEdge = innerHalfSize - std::max(dx, dy);
                    const float sampleFillCoverage = std::min(outerCoverage, edgeCoverage(innerDistanceToEdge));
                    fillCoverage += sampleFillCoverage * kSampleWeight;
                    outlineCoverage += std::max(0.0f, outerCoverage - sampleFillCoverage) * kSampleWeight;
                }
            }

            const float totalCoverage = std::clamp(fillCoverage + outlineCoverage, 0.0f, 1.0f);
            if (totalCoverage <= 0.0f) {
                continue;
            }

            const float fillContribution = fillCoverage / totalCoverage;
            const BYTE alpha = static_cast<BYTE>(std::lround(totalCoverage * 255.0f));
            const BYTE red = static_cast<BYTE>(std::lround(static_cast<float>(GetRValue(fillColor)) * fillContribution));
            const BYTE green = static_cast<BYTE>(std::lround(static_cast<float>(GetGValue(fillColor)) * fillContribution));
            const BYTE blue = static_cast<BYTE>(std::lround(static_cast<float>(GetBValue(fillColor)) * fillContribution));
            pixels[(y * cursorSize) + x] = alphaPixel(alpha, red, green, blue);
        }
    }

    HBITMAP maskBitmap = CreateBitmap(cursorSize, cursorSize, 1, 1, nullptr);
    if (maskBitmap == nullptr) {
        DeleteObject(colorBitmap);
        return nullptr;
    }

    ICONINFO iconInfo {};
    iconInfo.fIcon = FALSE;
    iconInfo.xHotspot = hotspot;
    iconInfo.yHotspot = hotspot;
    iconInfo.hbmMask = maskBitmap;
    iconInfo.hbmColor = colorBitmap;
    HCURSOR cursor = CreateIconIndirect(&iconInfo);

    DeleteObject(maskBitmap);
    DeleteObject(colorBitmap);
    return cursor;
}

std::wstring BaseName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return path;
    }

    return path.substr(slash + 1);
}

std::wstring EscapeMenuText(std::wstring_view value) {
    std::wstring escaped;
    escaped.reserve(value.size());
    for (wchar_t ch : value) {
        if (ch == L'&') {
            escaped.push_back(L'&');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::wstring RecentDocumentMenuLabel(const std::wstring& path) {
    std::wstring label;
    std::wstring fileName = BaseName(path);
    if (fileName.empty()) {
        fileName = path;
    }
    label += EscapeMenuText(fileName);

    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        label += L"\t";
        label += EscapeMenuText(parent.native());
    }
    return label;
}

float SnapBrushSize(float size) {
    constexpr float kMinBrushSize = 0.5f;
    constexpr float kMaxBrushSize = 20.0f;
    constexpr float kBrushSizeStep = 0.25f;
    const float clamped = std::clamp(size, kMinBrushSize, kMaxBrushSize);
    const float steps = std::round((clamped - kMinBrushSize) / kBrushSizeStep);
    return kMinBrushSize + (steps * kBrushSizeStep);
}

std::wstring FormatZoomPercentValue(float zoom) {
    wchar_t buffer[32] = {};
    const float percent = zoom * 100.0f;
    const bool wholePercent = std::abs(percent - std::round(percent)) < 0.05f;
    if (wholePercent) {
        std::swprintf(buffer, std::size(buffer), L"%.0f", percent);
    } else {
        std::swprintf(buffer, std::size(buffer), L"%.1f", percent);
    }
    std::wstring value = buffer;
    if (!wholePercent) {
        while (!value.empty() && value.back() == L'0') {
            value.pop_back();
        }
        if (!value.empty() && value.back() == L'.') {
            value.pop_back();
        }
    }
    return value;
}

int MeasureToolbarTextWidth(HDC hdc, const std::wstring& text) {
    if (text.empty()) {
        return 0;
    }

    SIZE size {};
    if (GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size) == FALSE) {
        return 0;
    }
    return static_cast<int>(size.cx);
}

ToolbarTitleLayout ResolveToolbarTitleLayout(const ToolbarLayout& layout, HDC hdc, const std::wstring& title, const std::wstring& zoomLabel, UINT dpi) {
    ToolbarTitleLayout resolved {};
    if (!HasRect(layout.titleAreaRect)) {
        return resolved;
    }

    const int kTitleLeftInset = ScaleForDpi(4, dpi);
    const int kTitleRightInset = ScaleForDpi(4, dpi);
    const int kZoomGap = ScaleForDpi(12, dpi);
    const int kZoomHorizontalPadding = ScaleForDpi(6, dpi);
    const int kMinimumTitleWidth = ScaleForDpi(24, dpi);
    const int kMinimumZoomWidth = ScaleForDpi(44, dpi);
    const int kMaximumZoomWidth = ScaleForDpi(72, dpi);

    const int availableWidth = static_cast<int>(layout.titleAreaRect.right - layout.titleAreaRect.left);
    const int zoomWidth = std::clamp(
        MeasureToolbarTextWidth(hdc, zoomLabel) + (kZoomHorizontalPadding * 2),
        kMinimumZoomWidth,
        kMaximumZoomWidth);
    const int maxTitleWidth = std::max(kMinimumTitleWidth, availableWidth - kZoomGap - zoomWidth);
    const int titleWidth = std::clamp(
        MeasureToolbarTextWidth(hdc, title) + kTitleLeftInset + kTitleRightInset,
        kMinimumTitleWidth,
        maxTitleWidth);

    resolved.fileTitleRect = MakeRect(
        layout.titleAreaRect.left,
        layout.titleAreaRect.top,
        layout.titleAreaRect.left + titleWidth,
        layout.titleAreaRect.bottom);
    resolved.zoomRect = MakeRect(
        resolved.fileTitleRect.right + kZoomGap,
        layout.titleAreaRect.top,
        resolved.fileTitleRect.right + kZoomGap + zoomWidth,
        layout.titleAreaRect.bottom);
    if (!HasRect(resolved.fileTitleRect) || !HasRect(resolved.zoomRect)) {
        resolved = {};
        return resolved;
    }

    resolved.titleTextRect = resolved.fileTitleRect;
    resolved.titleTextRect.left += kTitleLeftInset;
    resolved.titleTextRect.right -= kTitleRightInset;

    resolved.zoomTextRect = resolved.zoomRect;
    resolved.zoomTextRect.left += kZoomHorizontalPadding;
    resolved.zoomTextRect.right -= kZoomHorizontalPadding;
    resolved.dividerX = resolved.fileTitleRect.right + (kZoomGap / 2);
    return resolved;
}

struct SvgPathStyle {
    bool fill = true;
    bool stroke = false;
    float strokeWidth = 1.0f;
    Gdiplus::LineCap lineCap = Gdiplus::LineCapRound;
    Gdiplus::LineJoin lineJoin = Gdiplus::LineJoinRound;
};

struct SvgPathShape {
    std::unique_ptr<Gdiplus::GraphicsPath> path;
    SvgPathStyle style {};
};

struct SvgIconData {
    float viewBoxX = 0.0f;
    float viewBoxY = 0.0f;
    float viewBoxWidth = 24.0f;
    float viewBoxHeight = 24.0f;
    std::vector<SvgPathShape> shapes;
    bool loaded = false;
};

const wchar_t* ToolSvgFileName(App::Tool tool) {
    switch (tool) {
    case App::Tool::Select:
        return L"lasso.svg";
    case App::Tool::Move:
        return L"cursor.svg";
    case App::Tool::Text:
        return L"text.svg";
    case App::Tool::Draw:
        return L"draw.svg";
    case App::Tool::Pan:
        return L"pan.svg";
    case App::Tool::Erase:
        return L"erase.svg";
    default:
        return L"";
    }
}

std::optional<std::string> ReadUtf8TextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    input.seekg(0, std::ios::beg);

    std::string contents(static_cast<std::size_t>(size), '\0');
    if (!contents.empty()) {
        input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!input) {
            return std::nullopt;
        }
    }

    return contents;
}

std::optional<std::string> ExtractXmlAttribute(std::string_view tag, std::string_view attribute) {
    const std::string needle = std::string(attribute) + "=\"";
    const std::size_t start = tag.find(needle);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }

    const std::size_t valueStart = start + needle.size();
    const std::size_t valueEnd = tag.find('"', valueStart);
    if (valueEnd == std::string_view::npos) {
        return std::nullopt;
    }

    return std::string(tag.substr(valueStart, valueEnd - valueStart));
}

void SkipSvgSeparators(const char*& cursor) {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n' || *cursor == ',') {
        ++cursor;
    }
}

bool ConsumeSvgFloat(const char*& cursor, float& value) {
    SkipSvgSeparators(cursor);
    if (*cursor == '\0') {
        return false;
    }

    char* end = nullptr;
    value = std::strtof(cursor, &end);
    if (end == cursor) {
        return false;
    }
    cursor = end;
    SkipSvgSeparators(cursor);
    return true;
}

bool IsSvgCommandLetter(char ch) {
    switch (ch) {
    case 'M':
    case 'm':
    case 'L':
    case 'l':
    case 'H':
    case 'h':
    case 'V':
    case 'v':
    case 'C':
    case 'c':
    case 'A':
    case 'a':
    case 'Z':
    case 'z':
        return true;
    default:
        return false;
    }
}

bool AppendSvgArc(
    Gdiplus::GraphicsPath& path,
    const Gdiplus::PointF& start,
    float rx,
    float ry,
    float xAxisRotationDegrees,
    bool largeArcFlag,
    bool sweepFlag,
    const Gdiplus::PointF& end) {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kHalfPi = kPi / 2.0;

    if ((std::abs(start.X - end.X) < 0.0001f && std::abs(start.Y - end.Y) < 0.0001f)) {
        return true;
    }

    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx <= 0.0001f || ry <= 0.0001f) {
        path.AddLine(start, end);
        return true;
    }

    const double phi = static_cast<double>(xAxisRotationDegrees) * (kPi / 180.0);
    const double cosPhi = std::cos(phi);
    const double sinPhi = std::sin(phi);
    const double x1 = static_cast<double>(start.X);
    const double y1 = static_cast<double>(start.Y);
    const double x2 = static_cast<double>(end.X);
    const double y2 = static_cast<double>(end.Y);

    const double dx2 = (x1 - x2) * 0.5;
    const double dy2 = (y1 - y2) * 0.5;
    const double x1Prime = (cosPhi * dx2) + (sinPhi * dy2);
    const double y1Prime = (-sinPhi * dx2) + (cosPhi * dy2);

    double rxAdjusted = static_cast<double>(rx);
    double ryAdjusted = static_cast<double>(ry);
    const double rxSquared = rxAdjusted * rxAdjusted;
    const double rySquared = ryAdjusted * ryAdjusted;
    const double x1PrimeSquared = x1Prime * x1Prime;
    const double y1PrimeSquared = y1Prime * y1Prime;

    const double radiiScale = (x1PrimeSquared / rxSquared) + (y1PrimeSquared / rySquared);
    if (radiiScale > 1.0) {
        const double scale = std::sqrt(radiiScale);
        rxAdjusted *= scale;
        ryAdjusted *= scale;
    }

    const double adjustedRxSquared = rxAdjusted * rxAdjusted;
    const double adjustedRySquared = ryAdjusted * ryAdjusted;
    const double numerator = (adjustedRxSquared * adjustedRySquared)
        - (adjustedRxSquared * y1PrimeSquared)
        - (adjustedRySquared * x1PrimeSquared);
    const double denominator = (adjustedRxSquared * y1PrimeSquared) + (adjustedRySquared * x1PrimeSquared);
    const double centerFactor = denominator <= 0.0
        ? 0.0
        : std::sqrt(std::max(0.0, numerator / denominator)) * (largeArcFlag == sweepFlag ? -1.0 : 1.0);

    const double cxPrime = centerFactor * ((rxAdjusted * y1Prime) / ryAdjusted);
    const double cyPrime = centerFactor * (-(ryAdjusted * x1Prime) / rxAdjusted);
    const double centerX = (cosPhi * cxPrime) - (sinPhi * cyPrime) + ((x1 + x2) * 0.5);
    const double centerY = (sinPhi * cxPrime) + (cosPhi * cyPrime) + ((y1 + y2) * 0.5);

    const double startVectorX = (x1Prime - cxPrime) / rxAdjusted;
    const double startVectorY = (y1Prime - cyPrime) / ryAdjusted;
    const double endVectorX = (-x1Prime - cxPrime) / rxAdjusted;
    const double endVectorY = (-y1Prime - cyPrime) / ryAdjusted;

    double theta1 = std::atan2(startVectorY, startVectorX);
    double deltaTheta = std::atan2(
        (startVectorX * endVectorY) - (startVectorY * endVectorX),
        (startVectorX * endVectorX) + (startVectorY * endVectorY));

    if (!sweepFlag && deltaTheta > 0.0) {
        deltaTheta -= 2.0 * kPi;
    } else if (sweepFlag && deltaTheta < 0.0) {
        deltaTheta += 2.0 * kPi;
    }

    const int segmentCount = std::max(1, static_cast<int>(std::ceil(std::abs(deltaTheta) / kHalfPi)));
    const double segmentTheta = deltaTheta / static_cast<double>(segmentCount);

    auto transformPoint = [&](double localX, double localY) {
        return Gdiplus::PointF(
            static_cast<Gdiplus::REAL>((cosPhi * localX) - (sinPhi * localY) + centerX),
            static_cast<Gdiplus::REAL>((sinPhi * localX) + (cosPhi * localY) + centerY));
    };

    Gdiplus::PointF currentPoint = start;
    for (int segmentIndex = 0; segmentIndex < segmentCount; ++segmentIndex) {
        const double theta2 = theta1 + segmentTheta;
        const double alpha = (4.0 / 3.0) * std::tan((theta2 - theta1) * 0.25);
        const double cosTheta1 = std::cos(theta1);
        const double sinTheta1 = std::sin(theta1);
        const double cosTheta2 = std::cos(theta2);
        const double sinTheta2 = std::sin(theta2);

        const Gdiplus::PointF control1 = transformPoint(
            rxAdjusted * (cosTheta1 - (alpha * sinTheta1)),
            ryAdjusted * (sinTheta1 + (alpha * cosTheta1)));
        const Gdiplus::PointF control2 = transformPoint(
            rxAdjusted * (cosTheta2 + (alpha * sinTheta2)),
            ryAdjusted * (sinTheta2 - (alpha * cosTheta2)));
        const Gdiplus::PointF target = transformPoint(
            rxAdjusted * cosTheta2,
            ryAdjusted * sinTheta2);

        path.AddBezier(currentPoint, control1, control2, target);
        currentPoint = target;
        theta1 = theta2;
    }

    return true;
}

std::unique_ptr<Gdiplus::GraphicsPath> ParseSvgPathData(const std::string& pathData) {
    auto path = std::make_unique<Gdiplus::GraphicsPath>();
    const char* cursor = pathData.c_str();
    char command = '\0';
    Gdiplus::PointF current(0.0f, 0.0f);
    Gdiplus::PointF subpathStart(0.0f, 0.0f);
    bool hasCurrentPoint = false;

    while (*cursor != '\0') {
        SkipSvgSeparators(cursor);
        if (*cursor == '\0') {
            break;
        }

        if (IsSvgCommandLetter(*cursor)) {
            command = *cursor++;
        } else if (command == '\0') {
            return {};
        }

        switch (command) {
        case 'M':
        case 'm': {
            bool firstPoint = true;
            while (*cursor != '\0' && !IsSvgCommandLetter(*cursor)) {
                float x = 0.0f;
                float y = 0.0f;
                if (!ConsumeSvgFloat(cursor, x) || !ConsumeSvgFloat(cursor, y)) {
                    return {};
                }
                if (command == 'm' && hasCurrentPoint) {
                    x += current.X;
                    y += current.Y;
                }

                if (firstPoint) {
                    path->StartFigure();
                    current = Gdiplus::PointF(x, y);
                    subpathStart = current;
                    hasCurrentPoint = true;
                    firstPoint = false;
                } else {
                    path->AddLine(current, Gdiplus::PointF(x, y));
                    current = Gdiplus::PointF(x, y);
                }
            }
            break;
        }
        case 'L':
        case 'l':
            while (*cursor != '\0' && !IsSvgCommandLetter(*cursor)) {
                float x = 0.0f;
                float y = 0.0f;
                if (!ConsumeSvgFloat(cursor, x) || !ConsumeSvgFloat(cursor, y) || !hasCurrentPoint) {
                    return {};
                }
                if (command == 'l') {
                    x += current.X;
                    y += current.Y;
                }
                path->AddLine(current, Gdiplus::PointF(x, y));
                current = Gdiplus::PointF(x, y);
            }
            break;
        case 'H':
        case 'h':
            while (*cursor != '\0' && !IsSvgCommandLetter(*cursor)) {
                float x = 0.0f;
                if (!ConsumeSvgFloat(cursor, x) || !hasCurrentPoint) {
                    return {};
                }
                if (command == 'h') {
                    x += current.X;
                }
                path->AddLine(current, Gdiplus::PointF(x, current.Y));
                current = Gdiplus::PointF(x, current.Y);
            }
            break;
        case 'V':
        case 'v':
            while (*cursor != '\0' && !IsSvgCommandLetter(*cursor)) {
                float y = 0.0f;
                if (!ConsumeSvgFloat(cursor, y) || !hasCurrentPoint) {
                    return {};
                }
                if (command == 'v') {
                    y += current.Y;
                }
                path->AddLine(current, Gdiplus::PointF(current.X, y));
                current = Gdiplus::PointF(current.X, y);
            }
            break;
        case 'C':
        case 'c':
            while (*cursor != '\0' && !IsSvgCommandLetter(*cursor)) {
                float x1 = 0.0f;
                float y1 = 0.0f;
                float x2 = 0.0f;
                float y2 = 0.0f;
                float x3 = 0.0f;
                float y3 = 0.0f;
                if (!ConsumeSvgFloat(cursor, x1) || !ConsumeSvgFloat(cursor, y1)
                    || !ConsumeSvgFloat(cursor, x2) || !ConsumeSvgFloat(cursor, y2)
                    || !ConsumeSvgFloat(cursor, x3) || !ConsumeSvgFloat(cursor, y3)
                    || !hasCurrentPoint) {
                    return {};
                }
                if (command == 'c') {
                    x1 += current.X;
                    y1 += current.Y;
                    x2 += current.X;
                    y2 += current.Y;
                    x3 += current.X;
                    y3 += current.Y;
                }
                path->AddBezier(current, Gdiplus::PointF(x1, y1), Gdiplus::PointF(x2, y2), Gdiplus::PointF(x3, y3));
                current = Gdiplus::PointF(x3, y3);
            }
            break;
        case 'A':
        case 'a':
            while (*cursor != '\0' && !IsSvgCommandLetter(*cursor)) {
                float rx = 0.0f;
                float ry = 0.0f;
                float xAxisRotation = 0.0f;
                float largeArc = 0.0f;
                float sweep = 0.0f;
                float x = 0.0f;
                float y = 0.0f;
                if (!ConsumeSvgFloat(cursor, rx)
                    || !ConsumeSvgFloat(cursor, ry)
                    || !ConsumeSvgFloat(cursor, xAxisRotation)
                    || !ConsumeSvgFloat(cursor, largeArc)
                    || !ConsumeSvgFloat(cursor, sweep)
                    || !ConsumeSvgFloat(cursor, x)
                    || !ConsumeSvgFloat(cursor, y)
                    || !hasCurrentPoint) {
                    return {};
                }
                if (command == 'a') {
                    x += current.X;
                    y += current.Y;
                }
                const Gdiplus::PointF target(x, y);
                if (!AppendSvgArc(*path, current, rx, ry, xAxisRotation, largeArc >= 0.5f, sweep >= 0.5f, target)) {
                    return {};
                }
                current = target;
            }
            break;
        case 'Z':
        case 'z':
            if (hasCurrentPoint) {
                path->CloseFigure();
                current = subpathStart;
            }
            break;
        default:
            return {};
        }
    }

    return path;
}

std::optional<std::array<float, 4>> ParseViewBox(const std::string& viewBox) {
    const char* cursor = viewBox.c_str();
    std::array<float, 4> values {};
    for (float& value : values) {
        if (!ConsumeSvgFloat(cursor, value)) {
            return std::nullopt;
        }
    }
    return values;
}

Gdiplus::LineCap ParseSvgLineCap(const std::string& value) {
    if (value == "butt") {
        return Gdiplus::LineCapFlat;
    }
    if (value == "square") {
        return Gdiplus::LineCapSquare;
    }
    return Gdiplus::LineCapRound;
}

Gdiplus::LineJoin ParseSvgLineJoin(const std::string& value) {
    if (value == "bevel") {
        return Gdiplus::LineJoinBevel;
    }
    if (value == "miter") {
        return Gdiplus::LineJoinMiter;
    }
    return Gdiplus::LineJoinRound;
}

std::optional<SvgIconData> LoadSvgIcon(const std::wstring& fileName) {
    const std::array<std::filesystem::path, 3> basePaths {
        std::filesystem::current_path(),
        std::filesystem::path([] {
            wchar_t buffer[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            return std::filesystem::path(buffer).parent_path();
        }()),
        std::filesystem::path([] {
            wchar_t buffer[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            return std::filesystem::path(buffer).parent_path().parent_path();
        }()),
    };

    std::optional<std::string> svgContents;
    for (const auto& basePath : basePaths) {
        const auto candidate = basePath / "assets" / "icons" / fileName;
        svgContents = ReadUtf8TextFile(candidate);
        if (svgContents.has_value()) {
            break;
        }
    }

    if (!svgContents.has_value()) {
        return std::nullopt;
    }

    SvgIconData icon {};
    const std::size_t svgStart = svgContents->find("<svg");
    if (svgStart == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t svgEnd = svgContents->find('>', svgStart);
    if (svgEnd == std::string::npos) {
        return std::nullopt;
    }

    const std::string_view svgTag(svgContents->data() + svgStart, (svgEnd - svgStart) + 1U);
    if (const auto viewBox = ExtractXmlAttribute(svgTag, "viewBox"); viewBox.has_value()) {
        if (const auto parsed = ParseViewBox(*viewBox); parsed.has_value()) {
            icon.viewBoxX = (*parsed)[0];
            icon.viewBoxY = (*parsed)[1];
            icon.viewBoxWidth = (*parsed)[2];
            icon.viewBoxHeight = (*parsed)[3];
        }
    }

    std::size_t searchStart = 0;
    while (true) {
        const std::size_t pathStart = svgContents->find("<path", searchStart);
        if (pathStart == std::string::npos) {
            break;
        }
        const std::size_t pathEnd = svgContents->find('>', pathStart);
        if (pathEnd == std::string::npos) {
            return std::nullopt;
        }

        const std::string_view pathTag(svgContents->data() + pathStart, (pathEnd - pathStart) + 1U);
        const auto pathData = ExtractXmlAttribute(pathTag, "d");
        if (!pathData.has_value()) {
            searchStart = pathEnd + 1U;
            continue;
        }

        SvgPathShape shape {};
        shape.path = ParseSvgPathData(*pathData);
        if (!shape.path) {
            return std::nullopt;
        }

        if (const auto fill = ExtractXmlAttribute(pathTag, "fill"); fill.has_value()) {
            shape.style.fill = *fill != "none";
        }
        if (const auto stroke = ExtractXmlAttribute(pathTag, "stroke"); stroke.has_value()) {
            shape.style.stroke = *stroke != "none";
        }
        if (const auto strokeWidth = ExtractXmlAttribute(pathTag, "stroke-width"); strokeWidth.has_value()) {
            const char* cursor = strokeWidth->c_str();
            float parsedWidth = 1.0f;
            if (ConsumeSvgFloat(cursor, parsedWidth)) {
                shape.style.strokeWidth = parsedWidth;
            }
        }
        if (const auto lineCap = ExtractXmlAttribute(pathTag, "stroke-linecap"); lineCap.has_value()) {
            shape.style.lineCap = ParseSvgLineCap(*lineCap);
        }
        if (const auto lineJoin = ExtractXmlAttribute(pathTag, "stroke-linejoin"); lineJoin.has_value()) {
            shape.style.lineJoin = ParseSvgLineJoin(*lineJoin);
        }

        icon.shapes.push_back(std::move(shape));
        searchStart = pathEnd + 1U;
    }

    if (icon.shapes.empty()) {
        return std::nullopt;
    }

    icon.loaded = true;
    return icon;
}

const SvgIconData* ToolbarSvgIcon(std::wstring_view fileName) {
    struct CacheEntry {
        std::wstring name;
        std::optional<SvgIconData> icon;
    };

    static std::vector<CacheEntry> cache;
    const auto existing = std::find_if(cache.begin(), cache.end(), [&](const CacheEntry& entry) {
        return entry.name == fileName;
    });
    if (existing != cache.end()) {
        return existing->icon.has_value() ? &*existing->icon : nullptr;
    }

    cache.push_back({ std::wstring(fileName), LoadSvgIcon(std::wstring(fileName)) });
    return cache.back().icon.has_value() ? &*cache.back().icon : nullptr;
}

void PaintSvgIcon(HDC hdc, RECT rect, const SvgIconData& icon, COLORREF color) {
    if (!icon.loaded || icon.shapes.empty() || icon.viewBoxWidth <= 0.0f || icon.viewBoxHeight <= 0.0f) {
        return;
    }

    const float availableWidth = static_cast<float>((rect.right - rect.left) - 8);
    const float availableHeight = static_cast<float>((rect.bottom - rect.top) - 8);
    const float scale = std::max(0.01f, std::min(availableWidth / icon.viewBoxWidth, availableHeight / icon.viewBoxHeight));
    const float drawWidth = icon.viewBoxWidth * scale;
    const float drawHeight = icon.viewBoxHeight * scale;
    const float offsetX = static_cast<float>(rect.left) + ((rect.right - rect.left) - drawWidth) * 0.5f;
    const float offsetY = static_cast<float>(rect.top) + ((rect.bottom - rect.top) - drawHeight) * 0.5f;
    const auto alpha = static_cast<BYTE>(0xFF);
    const auto red = GetRValue(color);
    const auto green = GetGValue(color);
    const auto blue = GetBValue(color);
    const Gdiplus::Color gdipColor(alpha, red, green, blue);

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    for (const SvgPathShape& shape : icon.shapes) {
        std::unique_ptr<Gdiplus::GraphicsPath> transformed(shape.path->Clone());
        if (!transformed) {
            continue;
        }

        Gdiplus::Matrix matrix(
            scale,
            0.0f,
            0.0f,
            scale,
            offsetX - (icon.viewBoxX * scale),
            offsetY - (icon.viewBoxY * scale));
        transformed->Transform(&matrix);

        if (shape.style.fill) {
            Gdiplus::SolidBrush brush(gdipColor);
            graphics.FillPath(&brush, transformed.get());
        }

        if (shape.style.stroke) {
            Gdiplus::Pen pen(gdipColor, std::max(1.0f, shape.style.strokeWidth * scale));
            pen.SetLineCap(shape.style.lineCap, shape.style.lineCap, Gdiplus::DashCapRound);
            pen.SetLineJoin(shape.style.lineJoin);
            graphics.DrawPath(&pen, transformed.get());
        }
    }
}

ToolbarLayout BuildToolbarLayout(RECT clientRect, std::size_t colorCount, bool maximized, App::Tool activeTool, UINT dpi) {
    ToolbarLayout layout;
    static_cast<void>(colorCount);
    static_cast<void>(activeTool);

    const int toolbarHeight = ToolbarHeight(maximized, dpi);
    const int topInset = ToolbarTopInset(maximized, dpi);
    const int titleBarInset = maximized ? WindowFrameThicknessX(dpi) : 0;
    const int buttonStripRight = clientRect.right - titleBarInset;
    const int availableWidth = clientRect.right - clientRect.left;
    const bool compactTitle = availableWidth < ScaleForDpi(760, dpi);
    const int toolGap = ScaleForDpi(compactTitle ? 8 : 10, dpi);
    const int outerGap = ScaleForDpi(compactTitle ? 12 : 18, dpi);
    const int captionButtonWidth = ScaleForDpi(35, dpi);

    layout.closeRect = MakeRect(buttonStripRight - captionButtonWidth, topInset, buttonStripRight, toolbarHeight);
    layout.maximizeRect = MakeRect(layout.closeRect.left - captionButtonWidth, topInset, layout.closeRect.left, toolbarHeight);
    layout.minimizeRect = MakeRect(layout.maximizeRect.left - captionButtonWidth, topInset, layout.maximizeRect.left, toolbarHeight);

    const int rightEdge = buttonStripRight - CaptionButtonReserve(dpi);
    const int toolbarTop = topInset + ((ScaleForDpi(kTitleBarButtonHeight, dpi) - ScaleForDpi(kIconButtonSize, dpi)) / 2);
    const int colorButtonSize = ScaleForDpi(kColorSwatchSize, dpi);
    const int colorTop = topInset + ((ScaleForDpi(kTitleBarButtonHeight, dpi) - colorButtonSize) / 2);
    const int colorsLeft = rightEdge - colorButtonSize;

    layout.currentColorRect = MakeRect(colorsLeft, colorTop, colorsLeft + colorButtonSize, colorTop + colorButtonSize);

    const int toolBandRight = colorsLeft - outerGap;
    layout.panToolRect = MakeRect(
        toolBandRight - ScaleForDpi(kIconButtonSize, dpi),
        toolbarTop,
        toolBandRight,
        toolbarTop + ScaleForDpi(kIconButtonSize, dpi));
    layout.drawToolRect = MakeRect(
        layout.panToolRect.left - toolGap - ScaleForDpi(kIconButtonSize, dpi),
        toolbarTop,
        layout.panToolRect.left - toolGap,
        toolbarTop + ScaleForDpi(kIconButtonSize, dpi));
    layout.eraseToolRect = MakeRect(
        layout.drawToolRect.left - toolGap - ScaleForDpi(kIconButtonSize, dpi),
        toolbarTop,
        layout.drawToolRect.left - toolGap,
        toolbarTop + ScaleForDpi(kIconButtonSize, dpi));
    layout.textToolRect = MakeRect(
        layout.eraseToolRect.left - toolGap - ScaleForDpi(kIconButtonSize, dpi),
        toolbarTop,
        layout.eraseToolRect.left - toolGap,
        toolbarTop + ScaleForDpi(kIconButtonSize, dpi));
    layout.selectToolRect = MakeRect(
        layout.textToolRect.left - toolGap - ScaleForDpi(kIconButtonSize, dpi),
        toolbarTop,
        layout.textToolRect.left - toolGap,
        toolbarTop + ScaleForDpi(kIconButtonSize, dpi));
    layout.moveToolRect = MakeRect(
        layout.selectToolRect.left - toolGap - ScaleForDpi(kIconButtonSize, dpi),
        toolbarTop,
        layout.selectToolRect.left - toolGap,
        toolbarTop + ScaleForDpi(kIconButtonSize, dpi));
    const int redoRight = layout.moveToolRect.left - ScaleForDpi(compactTitle ? 12 : 16, dpi);
    const int iconButtonSize = ScaleForDpi(kIconButtonSize, dpi);
    const RECT candidateRedoRect = MakeRect(redoRight - iconButtonSize, toolbarTop, redoRight, toolbarTop + iconButtonSize);
    const RECT candidateUndoRect = MakeRect(candidateRedoRect.left - toolGap - iconButtonSize, toolbarTop, candidateRedoRect.left - toolGap, toolbarTop + iconButtonSize);
    const int titleLeft = ScaleForDpi(12, dpi);
    const int minimumTitleAreaWidth = ScaleForDpi(compactTitle ? 150 : 210, dpi);
    if (candidateUndoRect.left >= titleLeft + minimumTitleAreaWidth) {
        layout.redoRect = candidateRedoRect;
        layout.undoRect = candidateUndoRect;
    }

    const int titleRight = (HasRect(layout.undoRect) ? layout.undoRect.left : layout.moveToolRect.left) - ScaleForDpi(16, dpi);
    const int titleAreaWidth = std::max(0, titleRight - titleLeft);
    if (titleAreaWidth >= minimumTitleAreaWidth) {
        layout.titleAreaRect = MakeRect(titleLeft, topInset, titleRight, toolbarHeight);
    }

    return layout;
}

SidebarHeaderLayout BuildSidebarHeaderLayout(RECT sidebarRect, UINT dpi, bool maximized) {
    SidebarHeaderLayout layout {};
    const int padding = ScaleForDpi(6, dpi);
    const int gap = ScaleForDpi(4, dpi);
    const int actionHeight = ScaleForDpi(30, dpi);
    const int headerHeight = ToolbarHeight(maximized, dpi);
    layout.headerRect = MakeRect(sidebarRect.left + padding, sidebarRect.bottom - headerHeight, sidebarRect.right - padding, sidebarRect.bottom);
    const int rowTop = layout.headerRect.top + ((headerHeight - actionHeight) / 2);
    const int headerWidth = static_cast<int>(layout.headerRect.right - layout.headerRect.left);
    const int pageLabelWidth = std::clamp(
        headerWidth - (ScaleForDpi(52, dpi) * 2) - (gap * 2),
        ScaleForDpi(52, dpi),
        ScaleForDpi(64, dpi));
    const int actionWidth = std::max(ScaleForDpi(1, dpi), (headerWidth - pageLabelWidth - (gap * 2)) / 2);
    const int rowWidth = pageLabelWidth + (actionWidth * 2) + (gap * 2);
    const int rowLeft = layout.headerRect.left + std::max(0, (headerWidth - rowWidth) / 2);
    layout.pageLabelRect = MakeRect(rowLeft, rowTop, rowLeft + pageLabelWidth, rowTop + actionHeight);
    layout.addPageRect = MakeRect(layout.pageLabelRect.right + gap, rowTop, layout.pageLabelRect.right + gap + actionWidth, rowTop + actionHeight);
    layout.newPageSettingsRect = MakeRect(layout.addPageRect.right + gap, rowTop, layout.addPageRect.right + gap + actionWidth, rowTop + actionHeight);
    layout.contentRect = MakeRect(sidebarRect.left, sidebarRect.top, sidebarRect.right, layout.headerRect.top);
    return layout;
}

QuickSizePopupLayout BuildQuickSizePopupLayout(RECT clientRect, POINT anchor, UINT dpi, int previewExtent) {
    QuickSizePopupLayout layout {};
    const int kPanelWidth = ScaleForDpi(188, dpi);
    const int kPanelHeight = ScaleForDpi(38, dpi);
    const int kPreviewBounds = std::max(ScaleForDpi(28, dpi), previewExtent);
    const int kCursorGap = ScaleForDpi(14, dpi);
    const int kPreviewGap = ScaleForDpi(5, dpi);
    const int totalHeight = kPreviewBounds + kPreviewGap + kPanelHeight;
    const int toolbarBottom = clientRect.top;
    const int minLeft = static_cast<int>(clientRect.left) + ScaleForDpi(8, dpi);
    const int maxLeft = std::max(minLeft, static_cast<int>(clientRect.right) - kPanelWidth - ScaleForDpi(8, dpi));
    const int minTop = toolbarBottom + ScaleForDpi(8, dpi);
    const int maxTop = std::max(minTop, static_cast<int>(clientRect.bottom) - totalHeight - ScaleForDpi(8, dpi));
    int left = anchor.x - (kPanelWidth / 2);
    int top = anchor.y - totalHeight - kCursorGap;
    left = std::clamp(left, minLeft, maxLeft);
    top = std::clamp(top, minTop, maxTop);
    const int panelTop = top + kPreviewBounds + kPreviewGap;
    layout.panelRect = MakeRect(left, panelTop, left + kPanelWidth, panelTop + kPanelHeight);
    layout.sliderRect = MakeRect(left + ScaleForDpi(10, dpi), panelTop + ScaleForDpi(10, dpi), left + ScaleForDpi(144, dpi), panelTop + ScaleForDpi(28, dpi));
    layout.valueRect = MakeRect(left + ScaleForDpi(150, dpi), panelTop + ScaleForDpi(6, dpi), left + ScaleForDpi(178, dpi), panelTop + ScaleForDpi(32, dpi));
    const int sliderCenterX = (layout.sliderRect.left + layout.sliderRect.right) / 2;
    layout.previewRect = MakeRect(
        sliderCenterX - (kPreviewBounds / 2),
        top,
        sliderCenterX + (kPreviewBounds / 2),
        top + kPreviewBounds);
    return layout;
}

RECT QuickSizePopupBounds(const QuickSizePopupLayout& layout, UINT dpi) {
    return InflateRectCopy(UnionRectangles(layout.panelRect, layout.previewRect), ScaleForDpi(3, dpi));
}

TextFormatPopupLayout BuildTextFormatPopupLayout(RECT canvasRect, RECT editRect, bool fontMenuOpen, UINT dpi) {
    TextFormatPopupLayout layout {};
    const int panelPadding = ScaleForDpi(8, dpi);
    const int buttonSize = ScaleForDpi(28, dpi);
    const int textSizeInputWidth = ScaleForDpi(58, dpi);
    const int itemGap = ScaleForDpi(6, dpi);
    const int dropdownWidth = ScaleForDpi(118, dpi);
    const int panelHeight = ScaleForDpi(44, dpi);
    const int panelWidth = (panelPadding * 2) + (buttonSize * 4) + textSizeInputWidth + (itemGap * 5) + dropdownWidth;
    const int panelGap = ScaleForDpi(10, dpi);
    const int canvasPadding = ScaleForDpi(8, dpi);
    int left = editRect.left;
    const int minLeft = static_cast<int>(canvasRect.left) + canvasPadding;
    const int maxLeft = std::max(minLeft, static_cast<int>(canvasRect.right) - panelWidth - canvasPadding);
    left = std::clamp(left, minLeft, maxLeft);

    int top = editRect.top - panelHeight - panelGap;
    if (top < canvasRect.top + canvasPadding) {
        top = editRect.bottom + panelGap;
    }
    const int minTop = static_cast<int>(canvasRect.top) + canvasPadding;
    const int maxTop = std::max(minTop, static_cast<int>(canvasRect.bottom) - panelHeight - canvasPadding);
    top = std::clamp(top, minTop, maxTop);

    layout.panelRect = MakeRect(left, top, left + panelWidth, top + panelHeight);
    int itemLeft = layout.panelRect.left + panelPadding;
    const int itemTop = layout.panelRect.top + ((panelHeight - buttonSize) / 2);
    auto advanceButton = [&](RECT& rect) {
        rect = MakeRect(itemLeft, itemTop, itemLeft + buttonSize, itemTop + buttonSize);
        itemLeft = rect.right + itemGap;
    };

    advanceButton(layout.alignCycleRect);
    advanceButton(layout.boldRect);
    advanceButton(layout.italicRect);
    advanceButton(layout.underlineRect);
    layout.textSizeInputRect = MakeRect(itemLeft, itemTop, itemLeft + textSizeInputWidth, itemTop + buttonSize);
    itemLeft = layout.textSizeInputRect.right + itemGap;
    layout.fontDropdownRect = MakeRect(itemLeft, itemTop, itemLeft + dropdownWidth, itemTop + buttonSize);

    if (fontMenuOpen) {
        const int optionHeight = ScaleForDpi(28, dpi);
        const int menuPadding = ScaleForDpi(6, dpi);
        const int menuGap = ScaleForDpi(6, dpi);
        const int menuHeight = (menuPadding * 2) + (optionHeight * 3);
        const int maxMenuTop = std::max(minTop, static_cast<int>(canvasRect.bottom) - menuHeight - canvasPadding);
        const int belowTop = std::clamp(static_cast<int>(layout.panelRect.bottom) + menuGap, minTop, maxMenuTop);
        const int aboveTop = std::clamp(static_cast<int>(layout.panelRect.top) - menuHeight - menuGap, minTop, maxMenuTop);
        const RECT belowRect = MakeRect(layout.fontDropdownRect.left, belowTop, layout.fontDropdownRect.right, belowTop + menuHeight);
        const bool canUseBelow = !RectsOverlap(belowRect, editRect)
            && belowRect.bottom <= canvasRect.bottom - canvasPadding;
        const int menuTop = canUseBelow ? belowTop : aboveTop;
        layout.fontMenuRect = MakeRect(layout.fontDropdownRect.left, menuTop, layout.fontDropdownRect.right, menuTop + menuHeight);
        for (std::size_t index = 0; index < layout.fontOptionRects.size(); ++index) {
            const int optionTop = layout.fontMenuRect.top + menuPadding + (static_cast<int>(index) * optionHeight);
            layout.fontOptionRects[index] = MakeRect(
                layout.fontMenuRect.left + menuPadding,
                optionTop,
                layout.fontMenuRect.right - menuPadding,
                optionTop + optionHeight);
        }
    }

    return layout;
}

PaletteMenuLayout BuildPaletteMenuLayout(const ToolbarLayout& toolbarLayout, std::size_t colorCount, UINT dpi) {
    PaletteMenuLayout layout;
    const int columns = std::min(4, std::max(1, static_cast<int>(colorCount)));
    const int swatchSize = ScaleForDpi(28, dpi);
    const int gap = ScaleForDpi(8, dpi);
    const int rows = std::max(1, static_cast<int>((colorCount + static_cast<std::size_t>(columns) - 1U) / static_cast<std::size_t>(columns)));
    const int panelPadding = ScaleForDpi(12, dpi);
    const int panelWidth = (columns * swatchSize) + ((columns - 1) * gap) + (panelPadding * 2);
    const int panelHeight = (rows * swatchSize) + ((rows - 1) * gap) + (panelPadding * 2);
    layout.panelRect = MakeRect(
        toolbarLayout.currentColorRect.right - panelWidth,
        toolbarLayout.currentColorRect.bottom + ScaleForDpi(8, dpi),
        toolbarLayout.currentColorRect.right,
        toolbarLayout.currentColorRect.bottom + ScaleForDpi(8, dpi) + panelHeight);

    layout.swatchRects.reserve(colorCount);
    for (std::size_t index = 0; index < colorCount; ++index) {
        const int column = static_cast<int>(index % static_cast<std::size_t>(columns));
        const int row = static_cast<int>(index / static_cast<std::size_t>(columns));
        const int left = layout.panelRect.left + panelPadding + (column * (swatchSize + gap));
        const int top = layout.panelRect.top + panelPadding + (row * (swatchSize + gap));
        layout.swatchRects.push_back(MakeRect(left, top, left + swatchSize, top + swatchSize));
    }

    return layout;
}

PaletteWheelLayout BuildPaletteWheelLayout(RECT clientRect, POINT requestedCenter, std::size_t colorCount, int toolbarHeight, UINT dpi) {
    PaletteWheelLayout layout;
    layout.innerRadius = ScaleForDpi(20, dpi);
    layout.outerRadius = colorCount <= 1U ? ScaleForDpi(40, dpi) : ScaleForDpi(86, dpi);
    layout.hitOuterRadius = layout.outerRadius + PaletteWheelHoverGrow(dpi);
    const int margin = layout.hitOuterRadius
        + PaletteWheelShadowInset(dpi)
        + static_cast<int>(std::ceil(PaletteWheelStrokeWidth(kPaletteWheelSelectedStrokeDip, dpi)))
        + ScaleForDpi(4, dpi);
    layout.center.x = std::clamp(static_cast<int>(requestedCenter.x), margin, static_cast<int>(clientRect.right) - margin);
    layout.center.y = std::clamp(static_cast<int>(requestedCenter.y), toolbarHeight + margin, static_cast<int>(clientRect.bottom) - margin);
    return layout;
}

std::optional<std::size_t> PaletteWheelIndexAtPoint(const PaletteWheelLayout& layout, POINT cursor, std::size_t colorCount) {
    if (colorCount == 0U) {
        return std::nullopt;
    }

    const float dx = static_cast<float>(cursor.x - layout.center.x);
    const float dy = static_cast<float>(cursor.y - layout.center.y);
    const float distanceSquared = (dx * dx) + (dy * dy);
    const float innerSquared = static_cast<float>(layout.innerRadius * layout.innerRadius);
    const int hitOuterRadius = layout.hitOuterRadius > 0 ? layout.hitOuterRadius : layout.outerRadius;
    const float outerSquared = static_cast<float>(hitOuterRadius * hitOuterRadius);

    if (distanceSquared < innerSquared || distanceSquared > outerSquared) {
        return std::nullopt;
    }

    if (colorCount == 1U) {
        return 0U;
    }

    constexpr float kWheelPi = 3.14159265358979323846f;
    float angle = std::atan2(dy, dx);
    angle += kWheelPi / 2.0f;
    if (angle < 0.0f) {
        angle += 2.0f * kWheelPi;
    }

    const float normalized = angle / (2.0f * kWheelPi);
    const std::size_t index = std::min(
        colorCount - 1U,
        static_cast<std::size_t>(std::floor(normalized * static_cast<float>(colorCount))));
    return index;
}

RECT ToolbarBrushTrackRect(const RECT& sliderRect, UINT dpi) {
    const int centerY = (sliderRect.top + sliderRect.bottom) / 2;
    return MakeRect(
        sliderRect.left + ScaleForDpi(10, dpi),
        centerY - ScaleForDpi(3, dpi),
        sliderRect.right - ScaleForDpi(10, dpi),
        centerY + ScaleForDpi(3, dpi));
}

int ToolbarBrushThumbCenterX(const RECT& sliderRect, float brushSize) {
    const int width = std::max(1, static_cast<int>(sliderRect.right - sliderRect.left));
    const float normalized = std::clamp((SnapBrushSize(brushSize) - 0.5f) / 19.5f, 0.0f, 1.0f);
    return sliderRect.left + static_cast<int>(std::lround(normalized * static_cast<float>(width - 1)));
}

void PaintRoundedPanel(HDC hdc, const RECT& rect, COLORREF fill, COLORREF border, int borderWidth = 1) {
    ui::FillRoundedRect(hdc, rect, fill);
    ui::StrokeRoundedRect(hdc, rect, border, borderWidth);
}

void PaintSvgOrFallbackLabel(HDC hdc, RECT rect, const wchar_t* svgFileName, const wchar_t* fallbackLabel, COLORREF color, HFONT fallbackFont) {
    if (const SvgIconData* icon = ToolbarSvgIcon(svgFileName); icon != nullptr) {
        PaintSvgIcon(hdc, rect, *icon, color);
        return;
    }

    SetTextColor(hdc, color);
    HGDIOBJ previousFont = nullptr;
    if (fallbackFont != nullptr) {
        previousFont = SelectObject(hdc, fallbackFont);
    }
    DrawTextW(hdc, fallbackLabel, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (previousFont != nullptr) {
        SelectObject(hdc, previousFont);
    }
}

void PaintRoundedSvgButton(
    HDC hdc,
    const RECT& rect,
    COLORREF fill,
    COLORREF border,
    COLORREF contentColor,
    const wchar_t* svgFileName,
    const wchar_t* fallbackLabel,
    HFONT fallbackFont,
    UINT dpi,
    int iconInsetDips = 0) {
    PaintRoundedPanel(hdc, rect, fill, border);
    RECT contentRect = rect;
    if (iconInsetDips > 0) {
        const int inset = ScaleForDpi(iconInsetDips, dpi);
        InflateRect(&contentRect, -inset, -inset);
    }
    PaintSvgOrFallbackLabel(hdc, contentRect, svgFileName, fallbackLabel, contentColor, fallbackFont);
}

void PaintVerticalCaret(HDC hdc, int x, int top, int bottom, COLORREF color) {
    HPEN caretPen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldCaretPen = SelectObject(hdc, caretPen);
    MoveToEx(hdc, x, top, nullptr);
    LineTo(hdc, x, bottom);
    SelectObject(hdc, oldCaretPen);
    DeleteObject(caretPen);
}

void PaintSelectionHandle(HDC hdc, int centerX, int centerY, UINT dpi) {
    const int handleHalf = SelectionHandleHalfSize(dpi);
    const RECT handleRect = MakeRect(centerX - handleHalf, centerY - handleHalf, centerX + handleHalf + 1, centerY + handleHalf + 1);
    ui::FillRoundedRect(hdc, handleRect, RGB(242, 247, 252));
    ui::StrokeRoundedRect(hdc, handleRect, RGB(83, 112, 145), std::max(1, ScaleForDpi(1, dpi)));
}

void DrawPolylineAA(HDC hdc, const POINT* points, int count, COLORREF color, float width, Gdiplus::DashStyle dashStyle = Gdiplus::DashStyleSolid) {
    if (points == nullptr || count < 2) {
        return;
    }

    std::vector<Gdiplus::PointF> pathPoints;
    pathPoints.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        pathPoints.push_back(Gdiplus::PointF(static_cast<Gdiplus::REAL>(points[index].x), static_cast<Gdiplus::REAL>(points[index].y)));
    }

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)), width);
    pen.SetDashStyle(dashStyle);
    graphics.DrawLines(&pen, pathPoints.data(), static_cast<INT>(pathPoints.size()));
}

void DrawRectangleAA(HDC hdc, const RECT& rect, COLORREF color, float width, Gdiplus::DashStyle dashStyle = Gdiplus::DashStyleSolid) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)), width);
    pen.SetDashStyle(dashStyle);
    graphics.DrawRectangle(
        &pen,
        static_cast<Gdiplus::REAL>(rect.left),
        static_cast<Gdiplus::REAL>(rect.top),
        static_cast<Gdiplus::REAL>(rect.right - rect.left),
        static_cast<Gdiplus::REAL>(rect.bottom - rect.top));
}

RECT LiveTextEditFrameRect(const RECT& editRect, UINT dpi) {
    RECT frameRect = editRect;
    InflateRect(&frameRect, ScaleForDpi(4, dpi), ScaleForDpi(4, dpi));
    return frameRect;
}

void FillSolidRect(HDC hdc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

void DrawDashedRect(HDC hdc, const RECT& rect, COLORREF color, int thickness, int dashLength, int gapLength) {
    if (!HasRect(rect) || thickness <= 0 || dashLength <= 0) {
        return;
    }

    const auto drawHorizontal = [&](int left, int right, int top) {
        for (int x = left; x < right; x += dashLength + gapLength) {
            const int dashRight = std::min(x + dashLength, right);
            FillSolidRect(hdc, MakeRect(x, top, dashRight, top + thickness), color);
        }
    };
    const auto drawVertical = [&](int top, int bottom, int left) {
        for (int y = top; y < bottom; y += dashLength + gapLength) {
            const int dashBottom = std::min(y + dashLength, bottom);
            FillSolidRect(hdc, MakeRect(left, y, left + thickness, dashBottom), color);
        }
    };

    drawHorizontal(rect.left, rect.right, rect.top);
    drawHorizontal(rect.left, rect.right, rect.bottom - thickness);
    drawVertical(rect.top, rect.bottom, rect.left);
    drawVertical(rect.top, rect.bottom, rect.right - thickness);
}

template <std::size_t N>
void FillSolidPolygon(HDC hdc, const std::array<POINT, N>& points, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, points.data(), static_cast<int>(points.size()));
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(brush);
}

void PaintSelectToolIcon(HDC hdc, RECT rect, COLORREF color) {
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    const RECT box = MakeRect(centerX - 8, centerY - 7, centerX + 8, centerY + 7);
    const int thickness = 2;
    const int handle = 3;

    FillSolidRect(hdc, MakeRect(box.left, box.top, box.right, box.top + thickness), color);
    FillSolidRect(hdc, MakeRect(box.left, box.bottom - thickness, box.right, box.bottom), color);
    FillSolidRect(hdc, MakeRect(box.left, box.top, box.left + thickness, box.bottom), color);
    FillSolidRect(hdc, MakeRect(box.right - thickness, box.top, box.right, box.bottom), color);

    FillSolidRect(hdc, MakeRect(box.left - 1, box.top - 1, box.left - 1 + handle, box.top - 1 + handle), color);
    FillSolidRect(hdc, MakeRect(box.right - handle + 1, box.top - 1, box.right + 1, box.top - 1 + handle), color);
    FillSolidRect(hdc, MakeRect(box.left - 1, box.bottom - handle + 1, box.left - 1 + handle, box.bottom + 1), color);
    FillSolidRect(hdc, MakeRect(box.right - handle + 1, box.bottom - handle + 1, box.right + 1, box.bottom + 1), color);
}

void PaintMoveToolIcon(HDC hdc, RECT rect, COLORREF color) {
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    const std::array<POINT, 7> arrow {
        POINT { centerX - 8, centerY - 9 },
        POINT { centerX - 8, centerY + 8 },
        POINT { centerX - 4, centerY + 4 },
        POINT { centerX - 1, centerY + 11 },
        POINT { centerX + 3, centerY + 9 },
        POINT { centerX, centerY + 2 },
        POINT { centerX + 7, centerY + 2 },
    };
    FillSolidPolygon(hdc, arrow, color);
}

void PaintTextToolIcon(HDC hdc, RECT rect, COLORREF color) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    HFONT font = CreateFontW(
        -14,
        0,
        0,
        0,
        FW_BOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        VARIABLE_PITCH,
        L"Segoe UI");
    HGDIOBJ previousFont = font != nullptr ? SelectObject(hdc, font) : nullptr;
    RECT textRect = rect;
    DrawTextW(hdc, L"T", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (previousFont != nullptr) {
        SelectObject(hdc, previousFont);
    }
    if (font != nullptr) {
        DeleteObject(font);
    }
}

void PaintDrawToolIcon(HDC hdc, RECT rect, COLORREF color) {
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    const std::array<POINT, 4> body {
        POINT { centerX - 7, centerY + 5 },
        POINT { centerX + 2, centerY - 8 },
        POINT { centerX + 7, centerY - 3 },
        POINT { centerX - 2, centerY + 10 },
    };
    const std::array<POINT, 3> tip {
        POINT { centerX - 4, centerY + 8 },
        POINT { centerX - 1, centerY + 12 },
        POINT { centerX - 7, centerY + 11 },
    };
    FillSolidPolygon(hdc, body, color);
    FillSolidPolygon(hdc, tip, color);
}

void PaintPanToolIcon(HDC hdc, RECT rect, COLORREF color) {
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    FillSolidRect(hdc, MakeRect(centerX - 5, centerY - 1, centerX + 5, centerY + 8), color);
    FillSolidRect(hdc, MakeRect(centerX - 7, centerY - 7, centerX - 4, centerY + 1), color);
    FillSolidRect(hdc, MakeRect(centerX - 3, centerY - 10, centerX, centerY + 1), color);
    FillSolidRect(hdc, MakeRect(centerX + 1, centerY - 9, centerX + 4, centerY + 1), color);
    FillSolidRect(hdc, MakeRect(centerX + 5, centerY - 5, centerX + 8, centerY + 2), color);
    const std::array<POINT, 4> thumb {
        POINT { centerX - 5, centerY + 1 },
        POINT { centerX - 11, centerY - 2 },
        POINT { centerX - 9, centerY + 4 },
        POINT { centerX - 5, centerY + 6 },
    };
    FillSolidPolygon(hdc, thumb, color);
}

void PaintEraseToolIcon(HDC hdc, RECT rect, COLORREF color) {
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    const std::array<POINT, 4> eraser {
        POINT { centerX - 7, centerY + 4 },
        POINT { centerX - 1, centerY - 8 },
        POINT { centerX + 8, centerY - 1 },
        POINT { centerX + 2, centerY + 10 },
    };
    FillSolidPolygon(hdc, eraser, color);
    FillSolidRect(hdc, MakeRect(centerX - 1, centerY + 5, centerX + 7, centerY + 8), RGB(15, 18, 24));
}

float DistanceSquared(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
}

Vec2 Lerp(Vec2 from, Vec2 to, float amount) {
    return {
        from.x + ((to.x - from.x) * amount),
        from.y + ((to.y - from.y) * amount),
    };
}

float Distance(Vec2 a, Vec2 b) {
    return std::sqrt(DistanceSquared(a, b));
}

void AppendStrokePointWithResampling(Stroke& stroke, Vec2 point, float spacing) {
    if (stroke.points.empty()) {
        stroke.points.push_back(point);
        return;
    }

    constexpr int kMaxInterpolatedPoints = 64;
    const float minSpacing = std::max(0.05f, spacing);
    const Vec2 from = stroke.points.back();
    const float distance = Distance(from, point);
    if (distance < minSpacing) {
        return;
    }

    const int segmentCount = std::min(
        kMaxInterpolatedPoints + 1,
        std::max(1, static_cast<int>(std::ceil(distance / minSpacing))));
    for (int segment = 1; segment <= segmentCount; ++segment) {
        const float t = static_cast<float>(segment) / static_cast<float>(segmentCount);
        stroke.points.push_back(Lerp(from, point, t));
    }
}

bool StraightLineStrokeModifierPressed() {
    return (GetKeyState('A') & 0x8000) != 0;
}

bool SnapLineModifierPressed() {
    return (GetKeyState(VK_SHIFT) & 0x8000) != 0;
}

bool DottedStrokeModifierPressed() {
    return (GetKeyState(VK_SPACE) & 0x8000) != 0;
}

bool OvalStrokeModifierPressed() {
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
}

Vec2 SnapLineEndpoint(Vec2 start, Vec2 end) {
    constexpr float kSnapAngleRadians = 3.14159265358979323846f / 12.0f;
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float distance = std::sqrt((dx * dx) + (dy * dy));
    if (distance <= 0.001f) {
        return end;
    }

    const float snappedAngle = std::round(std::atan2(dy, dx) / kSnapAngleRadians) * kSnapAngleRadians;
    return {
        start.x + (std::cos(snappedAngle) * distance),
        start.y + (std::sin(snappedAngle) * distance),
    };
}

std::vector<Vec2> BuildOvalStrokePoints(Vec2 start, Vec2 end, bool circle) {
    constexpr int kOvalSegmentCount = 72;
    float endX = end.x;
    float endY = end.y;
    if (circle) {
        const float dx = end.x - start.x;
        const float dy = end.y - start.y;
        const float side = std::max(std::abs(dx), std::abs(dy));
        endX = start.x + (dx < 0.0f ? -side : side);
        endY = start.y + (dy < 0.0f ? -side : side);
    }

    const float left = std::min(start.x, endX);
    const float top = std::min(start.y, endY);
    const float right = std::max(start.x, endX);
    const float bottom = std::max(start.y, endY);
    const float radiusX = (right - left) * 0.5f;
    const float radiusY = (bottom - top) * 0.5f;
    if (radiusX <= 0.001f || radiusY <= 0.001f) {
        return { start, { endX, endY } };
    }

    const float centerX = left + radiusX;
    const float centerY = top + radiusY;
    std::vector<Vec2> points;
    points.reserve(kOvalSegmentCount + 1);
    for (int index = 0; index <= kOvalSegmentCount; ++index) {
        const float angle = (static_cast<float>(index) / static_cast<float>(kOvalSegmentCount)) * 2.0f * 3.14159265358979323846f;
        points.push_back({
            centerX + (std::cos(angle) * radiusX),
            centerY + (std::sin(angle) * radiusY),
        });
    }
    return points;
}

float InputSpacingPageUnits(float pageSpacing, float pageScale) {
    constexpr float kMinScreenSpacing = 1.75f;
    constexpr float kMaxScreenSpacing = 3.0f;
    const float scale = std::max(0.1f, pageScale);
    const float screenSpacing = std::clamp(std::max(0.05f, pageSpacing) * scale, kMinScreenSpacing, kMaxScreenSpacing);
    return screenSpacing / scale;
}

std::array<Vec2, 4> ImageCorners(const PageImage& image);

Vec2 PointToVec2(POINT point) {
    return {
        static_cast<float>(point.x),
        static_cast<float>(point.y),
    };
}

Vec2 RemapPagePointBetweenViews(const App::PageView& fromView, const App::PageView& toView, Vec2 point) {
    const float screenX = static_cast<float>(fromView.rect.left) + (point.x * fromView.scale);
    const float screenY = static_cast<float>(fromView.rect.top) + (point.y * fromView.scale);
    return {
        (screenX - static_cast<float>(toView.rect.left)) / std::max(0.1f, toView.scale),
        (screenY - static_cast<float>(toView.rect.top)) / std::max(0.1f, toView.scale),
    };
}

bool StrokeBounds(const Stroke& stroke, float* minX, float* minY, float* maxX, float* maxY) {
    if (stroke.points.empty() || minX == nullptr || minY == nullptr || maxX == nullptr || maxY == nullptr) {
        return false;
    }

    *minX = stroke.points.front().x;
    *minY = stroke.points.front().y;
    *maxX = stroke.points.front().x;
    *maxY = stroke.points.front().y;
    for (const Vec2& point : stroke.points) {
        *minX = std::min(*minX, point.x);
        *minY = std::min(*minY, point.y);
        *maxX = std::max(*maxX, point.x);
        *maxY = std::max(*maxY, point.y);
    }

    return true;
}

void TranslateStrokePoints(Stroke& stroke, Vec2 delta) {
    for (Vec2& point : stroke.points) {
        point.x += delta.x;
        point.y += delta.y;
    }
}

Vec2 ClampStrokeDeltaToPage(const Stroke& stroke, const Page& page, Vec2 delta) {
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    if (!StrokeBounds(stroke, &minX, &minY, &maxX, &maxY)) {
        return {};
    }

    return {
        std::clamp(delta.x, -minX, page.width - maxX),
        std::clamp(delta.y, -minY, page.height - maxY),
    };
}

Vec2 ClampImageDeltaToPage(const PageImage& image, const Page& page, Vec2 delta) {
    float minX = page.width;
    float minY = page.height;
    float maxX = 0.0f;
    float maxY = 0.0f;
    for (const Vec2& corner : ImageCorners(image)) {
        minX = std::min(minX, corner.x);
        minY = std::min(minY, corner.y);
        maxX = std::max(maxX, corner.x);
        maxY = std::max(maxY, corner.y);
    }

    return {
        std::clamp(delta.x, -minX, page.width - maxX),
        std::clamp(delta.y, -minY, page.height - maxY),
    };
}

std::vector<Vec2> BuildSmoothedPoints(const std::vector<Vec2>& points, float strength) {
    if (points.size() <= 2U || strength <= 0.001f) {
        return points;
    }

    std::vector<Vec2> smoothed;
    smoothed.reserve(points.size());
    smoothed.push_back(points.front());

    const float sideWeight = 0.25f * std::clamp(strength, 0.0f, 1.0f);
    const float centerWeight = 1.0f - (sideWeight * 2.0f);

    for (std::size_t index = 1; index + 1 < points.size(); ++index) {
        const Vec2 previous = points[index - 1];
        const Vec2 current = points[index];
        const Vec2 next = points[index + 1];
        const Vec2 filtered {
            (previous.x * sideWeight) + (current.x * centerWeight) + (next.x * sideWeight),
            (previous.y * sideWeight) + (current.y * centerWeight) + (next.y * sideWeight),
        };

        if (DistanceSquared(filtered, smoothed.back()) >= 0.05f) {
            smoothed.push_back(filtered);
        }
    }

    if (DistanceSquared(points.back(), smoothed.back()) >= 0.01f) {
        smoothed.push_back(points.back());
    }

    return smoothed;
}

float StrokeRenderSmoothing(const Stroke& stroke, float fallbackSmoothing) {
    const float smoothing = std::isfinite(stroke.smoothing) ? stroke.smoothing : fallbackSmoothing;
    return std::clamp(smoothing, 0.0f, 1.0f);
}

std::vector<Vec2> StrokeRenderPoints(const Stroke& stroke, float fallbackSmoothing) {
    return BuildSmoothedPoints(stroke.points, StrokeRenderSmoothing(stroke, fallbackSmoothing));
}

std::optional<RECT> StrokeRenderBoundsRect(
    const Stroke& stroke,
    float scale,
    bool,
    float strokeSmoothing,
    int extraPadding = 0) {
    if (stroke.points.empty() || scale <= 0.0f) {
        return std::nullopt;
    }

    const std::vector<Vec2> renderPoints = StrokeRenderPoints(stroke, strokeSmoothing);
    if (renderPoints.empty()) {
        return std::nullopt;
    }

    float minX = renderPoints.front().x;
    float minY = renderPoints.front().y;
    float maxX = renderPoints.front().x;
    float maxY = renderPoints.front().y;
    for (const Vec2& point : renderPoints) {
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
        maxX = std::max(maxX, point.x);
        maxY = std::max(maxY, point.y);
    }

    const float penWidth = std::max(1.25f, stroke.style.width * scale / 2.15f);
    const float padding = static_cast<float>(static_cast<int>(std::ceil(penWidth)) + extraPadding);
    return MakeRectFromFloatBounds(
        (minX * scale) - padding,
        (minY * scale) - padding,
        (maxX * scale) + padding + 1.0f,
        (maxY * scale) + padding + 1.0f);
}

bool PointInRectF(Vec2 point, float left, float top, float right, float bottom) {
    return point.x >= left && point.x <= right && point.y >= top && point.y <= bottom;
}

float DistancePointToSegmentSquared(Vec2 point, Vec2 a, Vec2 b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float lengthSquared = (dx * dx) + (dy * dy);
    if (lengthSquared <= 0.0001f) {
        return DistanceSquared(point, a);
    }

    float t = ((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSquared;
    t = std::clamp(t, 0.0f, 1.0f);
    const Vec2 projection {
        a.x + (dx * t),
        a.y + (dy * t),
    };
    return DistanceSquared(point, projection);
}

float DistanceSegmentToSegmentSquared(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2) {
    if (SegmentsIntersect(a1, a2, b1, b2)) {
        return 0.0f;
    }

    return std::min({
        DistancePointToSegmentSquared(a1, b1, b2),
        DistancePointToSegmentSquared(a2, b1, b2),
        DistancePointToSegmentSquared(b1, a1, a2),
        DistancePointToSegmentSquared(b2, a1, a2),
    });
}

float CrossProduct(Vec2 a, Vec2 b, Vec2 c) {
    return ((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x));
}

bool OnSegment(Vec2 a, Vec2 b, Vec2 point) {
    return point.x >= std::min(a.x, b.x) - 0.001f
        && point.x <= std::max(a.x, b.x) + 0.001f
        && point.y >= std::min(a.y, b.y) - 0.001f
        && point.y <= std::max(a.y, b.y) + 0.001f;
}

bool RectContainsPointWithPadding(RECT rect, POINT point, int padding) {
    if (!HasRect(rect)) {
        return false;
    }

    padding = std::max(0, padding);
    InflateRect(&rect, padding, padding);
    return PtInRect(&rect, point) != 0;
}

bool PolygonContainsPoint(const std::vector<Vec2>& polygon, POINT point) {
    if (polygon.size() < 3U) {
        return false;
    }

    const Vec2 cursorPoint { static_cast<float>(point.x), static_cast<float>(point.y) };
    return PointInPolygon(cursorPoint, polygon);
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

    if (std::abs(d1) <= 0.001f && OnSegment(a1, a2, b1)) {
        return true;
    }
    if (std::abs(d2) <= 0.001f && OnSegment(a1, a2, b2)) {
        return true;
    }
    if (std::abs(d3) <= 0.001f && OnSegment(b1, b2, a1)) {
        return true;
    }
    if (std::abs(d4) <= 0.001f && OnSegment(b1, b2, a2)) {
        return true;
    }
    return false;
}

bool SegmentIntersectsRectF(Vec2 a, Vec2 b, float left, float top, float right, float bottom) {
    if (PointInRectF(a, left, top, right, bottom) || PointInRectF(b, left, top, right, bottom)) {
        return true;
    }

    return SegmentsIntersect(a, b, { left, top }, { right, top })
        || SegmentsIntersect(a, b, { right, top }, { right, bottom })
        || SegmentsIntersect(a, b, { right, bottom }, { left, bottom })
        || SegmentsIntersect(a, b, { left, bottom }, { left, top });
}

bool PointInPolygon(Vec2 point, const std::vector<Vec2>& polygon) {
    if (polygon.size() < 3U) {
        return false;
    }

    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1U; i < polygon.size(); j = i++) {
        const Vec2 a = polygon[i];
        const Vec2 b = polygon[j];
        const bool intersects = ((a.y > point.y) != (b.y > point.y))
            && (point.x < ((b.x - a.x) * (point.y - a.y) / ((b.y - a.y) + 0.00001f)) + a.x);
        if (intersects) {
            inside = !inside;
        }
    }
    return inside;
}

bool StrokeHitsPoint(const Stroke& stroke, Vec2 point, float radius, float fallbackSmoothing) {
    const float radiusSquared = radius * radius;
    const std::vector<Vec2> points = StrokeRenderPoints(stroke, fallbackSmoothing);
    if (points.empty()) {
        return false;
    }

    if (DistanceSquared(points.front(), point) <= radiusSquared) {
        return true;
    }

    for (std::size_t index = 1; index < points.size(); ++index) {
        if (DistancePointToSegmentSquared(point, points[index - 1], points[index]) <= radiusSquared) {
            return true;
        }
    }

    return false;
}

bool StrokeIntersectsRectSelection(const Stroke& stroke, Vec2 a, Vec2 b, float fallbackSmoothing) {
    const std::vector<Vec2> points = StrokeRenderPoints(stroke, fallbackSmoothing);
    if (points.empty()) {
        return false;
    }

    const float left = std::min(a.x, b.x);
    const float top = std::min(a.y, b.y);
    const float right = std::max(a.x, b.x);
    const float bottom = std::max(a.y, b.y);

    for (const Vec2& point : points) {
        if (PointInRectF(point, left, top, right, bottom)) {
            return true;
        }
    }

    for (std::size_t index = 1; index < points.size(); ++index) {
        if (SegmentIntersectsRectF(points[index - 1], points[index], left, top, right, bottom)) {
            return true;
        }
    }

    return false;
}

bool StrokeIntersectsPolygonSelection(const Stroke& stroke, const std::vector<Vec2>& polygon, float fallbackSmoothing) {
    const std::vector<Vec2> points = StrokeRenderPoints(stroke, fallbackSmoothing);
    if (points.empty() || polygon.size() < 3U) {
        return false;
    }

    for (const Vec2& point : points) {
        if (PointInPolygon(point, polygon)) {
            return true;
        }
    }

    for (std::size_t index = 1; index < points.size(); ++index) {
        const Vec2 strokeA = points[index - 1];
        const Vec2 strokeB = points[index];
        for (std::size_t edgeIndex = 0; edgeIndex < polygon.size(); ++edgeIndex) {
            const Vec2 polyA = polygon[edgeIndex];
            const Vec2 polyB = polygon[(edgeIndex + 1U) % polygon.size()];
            if (SegmentsIntersect(strokeA, strokeB, polyA, polyB)) {
                return true;
            }
        }
    }

    return false;
}

constexpr float kPi = 3.14159265358979323846f;

float DegreesToRadians(float degrees) {
    return degrees * (kPi / 180.0f);
}

Vec2 RotatePointAround(Vec2 point, Vec2 center, float degrees) {
    const float radians = DegreesToRadians(degrees);
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    const float translatedX = point.x - center.x;
    const float translatedY = point.y - center.y;
    return {
        center.x + (translatedX * cosine) - (translatedY * sine),
        center.y + (translatedX * sine) + (translatedY * cosine),
    };
}

Vec2 ImageCenter(const PageImage& image) {
    return {
        image.x + (image.width * 0.5f),
        image.y + (image.height * 0.5f),
    };
}

std::array<Vec2, 4> ImageCorners(const PageImage& image) {
    const Vec2 center = ImageCenter(image);
    std::array<Vec2, 4> corners {
        Vec2 { image.x, image.y },
        Vec2 { image.x + image.width, image.y },
        Vec2 { image.x + image.width, image.y + image.height },
        Vec2 { image.x, image.y + image.height },
    };
    for (Vec2& corner : corners) {
        corner = RotatePointAround(corner, center, image.rotationDegrees);
    }
    return corners;
}

Vec2 ScreenPointFromPagePoint(Vec2 point, RECT pageRect, float pageScale) {
    return {
        static_cast<float>(pageRect.left) + (point.x * pageScale),
        static_cast<float>(pageRect.top) + (point.y * pageScale),
    };
}

Vec2 ToImageLocalPoint(const PageImage& image, Vec2 point) {
    const Vec2 center = ImageCenter(image);
    return RotatePointAround(point, center, -image.rotationDegrees);
}

bool ImageHitsPoint(const PageImage& image, Vec2 point) {
    const Vec2 local = ToImageLocalPoint(image, point);
    return PointInRectF(local, image.x, image.y, image.x + image.width, image.y + image.height);
}

bool CircleHitsRect(float left, float top, float right, float bottom, Vec2 center, float radius) {
    const float closestX = std::clamp(center.x, left, right);
    const float closestY = std::clamp(center.y, top, bottom);
    const float dx = center.x - closestX;
    const float dy = center.y - closestY;
    return (dx * dx) + (dy * dy) <= radius * radius;
}

bool SegmentHitsRect(float left, float top, float right, float bottom, Vec2 a, Vec2 b, float radius) {
    if (CircleHitsRect(left, top, right, bottom, a, radius)
        || CircleHitsRect(left, top, right, bottom, b, radius)
        || SegmentIntersectsRectF(a, b, left, top, right, bottom)) {
        return true;
    }

    const float radiusSquared = radius * radius;
    return DistancePointToSegmentSquared({ left, top }, a, b) <= radiusSquared
        || DistancePointToSegmentSquared({ right, top }, a, b) <= radiusSquared
        || DistancePointToSegmentSquared({ right, bottom }, a, b) <= radiusSquared
        || DistancePointToSegmentSquared({ left, bottom }, a, b) <= radiusSquared;
}

bool ImageHitsSegment(const PageImage& image, Vec2 a, Vec2 b, float radius) {
    return SegmentHitsRect(
        image.x,
        image.y,
        image.x + image.width,
        image.y + image.height,
        ToImageLocalPoint(image, a),
        ToImageLocalPoint(image, b),
        radius);
}

bool TextHitsSegment(const PageText& text, float pageWidth, Vec2 a, Vec2 b, float radius) {
    const PageTextBounds bounds = MeasurePageTextBounds(text, pageWidth);
    return SegmentHitsRect(bounds.left, bounds.top, bounds.right, bounds.bottom, a, b, radius);
}

std::optional<RECT> ImageBoundsRect(const PageImage& image, float scale, int extraPadding = 0) {
    if (scale <= 0.0f) {
        return std::nullopt;
    }

    const auto corners = ImageCorners(image);
    float minX = corners.front().x;
    float minY = corners.front().y;
    float maxX = corners.front().x;
    float maxY = corners.front().y;
    for (const Vec2& corner : corners) {
        minX = std::min(minX, corner.x);
        minY = std::min(minY, corner.y);
        maxX = std::max(maxX, corner.x);
        maxY = std::max(maxY, corner.y);
    }

    const float padding = static_cast<float>(extraPadding);
    return MakeRectFromFloatBounds(
        (minX * scale) - padding,
        (minY * scale) - padding,
        (maxX * scale) + padding + 1.0f,
        (maxY * scale) + padding + 1.0f);
}

bool ImageIntersectsRectSelection(const PageImage& image, Vec2 a, Vec2 b) {
    const float left = std::min(a.x, b.x);
    const float top = std::min(a.y, b.y);
    const float right = std::max(a.x, b.x);
    const float bottom = std::max(a.y, b.y);
    const auto corners = ImageCorners(image);
    for (const Vec2& corner : corners) {
        if (PointInRectF(corner, left, top, right, bottom)) {
            return true;
        }
    }
    if (ImageHitsPoint(image, { left, top }) || ImageHitsPoint(image, { right, top })
        || ImageHitsPoint(image, { right, bottom }) || ImageHitsPoint(image, { left, bottom })) {
        return true;
    }
    for (std::size_t index = 0; index < corners.size(); ++index) {
        if (SegmentIntersectsRectF(corners[index], corners[(index + 1U) % corners.size()], left, top, right, bottom)) {
            return true;
        }
    }
    return false;
}

bool ImageIntersectsPolygonSelection(const PageImage& image, const std::vector<Vec2>& polygon) {
    if (polygon.size() < 3U) {
        return false;
    }

    const auto corners = ImageCorners(image);

    for (const Vec2& corner : corners) {
        if (PointInPolygon(corner, polygon)) {
            return true;
        }
    }

    for (const Vec2& point : polygon) {
        if (ImageHitsPoint(image, point)) {
            return true;
        }
    }

    for (std::size_t edgeIndex = 0; edgeIndex < polygon.size(); ++edgeIndex) {
        const Vec2 polyA = polygon[edgeIndex];
        const Vec2 polyB = polygon[(edgeIndex + 1U) % polygon.size()];
        for (std::size_t imageEdgeIndex = 0; imageEdgeIndex < corners.size(); ++imageEdgeIndex) {
            if (SegmentsIntersect(polyA, polyB, corners[imageEdgeIndex], corners[(imageEdgeIndex + 1U) % corners.size()])) {
                return true;
            }
        }
    }

    return false;
}

}  // namespace

App::App(HINSTANCE instance) : instance_(instance) {
    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) == Gdiplus::Ok) {
        gdiplusToken_ = token;
    }
    shortcuts_.bindings.reserve(ShortcutDefinitions().size());
    for (const ShortcutDefinition& definition : ShortcutDefinitions()) {
        shortcuts_.bindings.push_back({ definition.action, definition.defaultBinding });
    }
    LoadAppSettings();
    ClearSelection();
}

App::~App() {
    delete settings_.window;
    settings_.window = nullptr;
    delete newPageSettings_.window;
    newPageSettings_.window = nullptr;
    EndTextEditControl();
    SaveAppSettings();
    ClearZoomSessionSnapshots();
    ReleasePaintBackbuffer();
    ReleaseCompositionResources();
    StopSmoothScroll();
    ReleaseEraserCursor();
    ReleaseDrawCursor();
    if (gdiplusToken_ != 0) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
    }
    if (toolbarRevealTimerResolutionActive_) {
        timeEndPeriod(1);
        toolbarRevealTimerResolutionActive_ = false;
    }
    if (pdfSidebarSlideTimerResolutionActive_) {
        timeEndPeriod(1);
        pdfSidebarSlideTimerResolutionActive_ = false;
    }
}

int App::FullToolbarHeight() const {
    return ToolbarHeight(IsZoomed(window_), WindowDpi(window_));
}

bool App::IsToolbarVisible() const {
    if (!settings_.autoHideToolbar) {
        return true;
    }

    return toolbarRevealProgress_ > 0.001f
        || toolbarRevealed_
        || toolbarZoomEditing_
        || toolbarBrushSizeEditing_
        || toolbarBrushSliderDragging_
        || titleMenuActive_
        || wheel_.paletteMenuOpen;
}

int App::VisibleToolbarHeight() const {
    return IsToolbarVisible() ? FullToolbarHeight() : 0;
}

int App::ReservedToolbarHeight() const {
    return settings_.autoHideToolbar ? 0 : FullToolbarHeight();
}

void App::SetToolbarRevealed(bool revealed) {
    if (!settings_.autoHideToolbar) {
        revealed = true;
    }
    if (toolbarRevealed_ == revealed) {
        return;
    }

    toolbarRevealed_ = revealed;
    hoveredWindowButton_ = -1;
    hoveredTitleMenu_ = false;

    if (revealed) {
        StartToolbarRevealAnimation(1.0f);
    } else {
        StartToolbarRevealAnimation(0.0f);
    }
}

int App::ToolbarHiddenOvershoot() const {
    return ScaleForDpi(2, WindowDpi(window_));
}

int App::ToolbarHideAnimationOvershoot(int toolbarHeight) const {
    if (IsZoomed(window_)) {
        return ToolbarHiddenOvershoot();
    }
    return std::max(toolbarHeight * 4, ScaleForDpi(48, WindowDpi(window_)));
}

int App::ToolbarVisibleClipHeight(int toolbarHeight) const {
    const int edgeGuard = ScaleForDpi(2, WindowDpi(window_));
    return std::max(1, toolbarHeight - edgeGuard);
}

float App::ToolbarOffsetYForProgress(int toolbarHeight, float progress) const {
    progress = std::clamp(progress, 0.0f, 1.0f);
    return -static_cast<float>(toolbarHeight + ToolbarHiddenOvershoot()) * (1.0f - progress);
}

float App::ToolbarHideAnimationTargetOffsetY(int toolbarHeight) const {
    return -static_cast<float>(toolbarHeight + ToolbarHideAnimationOvershoot(toolbarHeight));
}

std::uint64_t App::ToolbarRenderSignature(RECT clientRect) const {
    constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
    std::uint64_t hash = kFnvOffset;
    const UINT dpi = WindowDpi(window_);
    const bool maximized = IsZoomed(window_) != FALSE;
    const int clientWidth = static_cast<int>(clientRect.right - clientRect.left);
    const int toolbarHeight = FullToolbarHeight();
    const int activeTool = static_cast<int>(activeTool_);
    const int hoveredWindowButton = hoveredWindowButton_;
    const bool canUndo = CanUndo();
    const bool canRedo = CanRedo();
    const bool movingToolSelected = activeTool_ == Tool::Move || isMovingSelection_ || isTransformingImage_ || isTransformingStroke_;
    const bool paletteMenuOpen = wheel_.paletteMenuOpen;
    const bool titleMenuOpen = titleMenuActive_;
    const bool titleHovered = hoveredTitleMenu_;
    const bool zoomEditing = toolbarZoomEditing_;
    const bool brushEditing = toolbarBrushSizeEditing_;
    const bool brushDragging = toolbarBrushSliderDragging_;

    HashToolbarValue(hash, dpi);
    HashToolbarValue(hash, maximized);
    HashToolbarValue(hash, clientWidth);
    HashToolbarValue(hash, toolbarHeight);
    HashToolbarValue(hash, activeTool);
    HashToolbarValue(hash, colorIndex_);
    HashToolbarValue(hash, hoveredWindowButton);
    HashToolbarValue(hash, canUndo);
    HashToolbarValue(hash, canRedo);
    HashToolbarValue(hash, movingToolSelected);
    HashToolbarValue(hash, paletteMenuOpen);
    HashToolbarValue(hash, titleMenuOpen);
    HashToolbarValue(hash, titleHovered);
    HashToolbarValue(hash, zoomEditing);
    HashToolbarValue(hash, brushEditing);
    HashToolbarValue(hash, brushDragging);
    HashToolbarString(hash, CurrentWindowTitle());
    HashToolbarString(hash, ZoomIndicatorLabel());
    HashToolbarString(hash, ToolbarBrushValueLabel());

    const std::size_t paletteSize = palette_.size();
    HashToolbarValue(hash, paletteSize);
    for (std::uint32_t color : palette_) {
        HashToolbarValue(hash, color);
    }

    const std::size_t wheelToolCount = wheel_.tools.size();
    HashToolbarValue(hash, wheelToolCount);
    for (Tool tool : wheel_.tools) {
        const int toolValue = static_cast<int>(tool);
        HashToolbarValue(hash, toolValue);
    }

    return hash;
}

void App::SetToolbarAutoHideEnabled(bool enabled) {
    if (settings_.autoHideToolbar == enabled) {
        return;
    }

    settings_.autoHideToolbar = enabled;
    toolbarRevealed_ = true;
    toolbarRevealProgress_ = 1.0f;
    toolbarRevealAnimationStartProgress_ = 1.0f;
    toolbarRevealAnimationTargetProgress_ = 1.0f;
    hoveredWindowButton_ = -1;
    hoveredTitleMenu_ = false;

    if (window_ != nullptr) {
        StopToolbarRevealAnimationTimer();
        InvalidatePdfSidebarAnimationRegion();
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::UpdateToolbarAutoHide(POINT cursor) {
    if (!settings_.autoHideToolbar) {
        SetToolbarRevealed(true);
        return;
    }
    if (toolbarZoomEditing_
        || toolbarBrushSizeEditing_
        || toolbarBrushSliderDragging_
        || titleMenuActive_
        || wheel_.paletteMenuOpen
        || isDrawing_
        || isErasing_
        || isPanning_
        || isSelecting_
        || pendingTextEditClick_
        || isMovingSelection_
        || isTransformingImage_
        || isTransformingText_
        || isTransformingStroke_) {
        return;
    }

    const UINT dpi = WindowDpi(window_);
    const int revealTop = IsZoomed(window_) ? -WindowFrameThicknessX(dpi) : 0;
    const int revealHeight = ScaleForDpi(IsZoomed(window_) ? 22 : 12, dpi);
    if (cursor.y >= revealTop && cursor.y < revealHeight) {
        SetToolbarRevealed(true);
        return;
    }

    const int toolbarHeight = FullToolbarHeight();
    if (toolbarRevealed_ && cursor.y < toolbarHeight) {
        return;
    }

    if (toolbarRevealed_) {
        SetToolbarRevealed(false);
    }
}

void App::StartToolbarRevealAnimation(float targetProgress) {
    targetProgress = std::clamp(targetProgress, 0.0f, 1.0f);
    if (!settings_.autoHideToolbar || window_ == nullptr) {
        toolbarRevealProgress_ = targetProgress;
        StopToolbarRevealAnimationTimer();
        if (window_ != nullptr) {
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            static_cast<void>(UpdateToolbarComposition(clientRect));
            static_cast<void>(UpdatePdfSidebarComposition(clientRect));
        }
        return;
    }

    if (toolbarRevealAnimationStartMs_ > 0) {
        const ULONGLONG nowMs = GetTickCount64();
        const double elapsed = static_cast<double>(nowMs - toolbarRevealAnimationStartMs_);
        const float linearT = static_cast<float>(std::clamp(elapsed / static_cast<double>(kToolbarRevealDurationMs), 0.0, 1.0));
        const float easedT = linearT * linearT * (3.0f - (2.0f * linearT));
        toolbarRevealProgress_ = toolbarRevealAnimationStartProgress_
            + ((toolbarRevealAnimationTargetProgress_ - toolbarRevealAnimationStartProgress_) * easedT);
    }
    StopToolbarRevealAnimationTimer();

    if (std::abs(toolbarRevealProgress_ - targetProgress) <= 0.001f) {
        toolbarRevealProgress_ = targetProgress;
        toolbarRevealAnimationStartProgress_ = targetProgress;
        toolbarRevealAnimationTargetProgress_ = targetProgress;
        toolbarRevealAnimationStartMs_ = 0;
    } else {
        toolbarRevealAnimationStartProgress_ = toolbarRevealProgress_;
        toolbarRevealAnimationTargetProgress_ = targetProgress;
        toolbarRevealAnimationStartMs_ = GetTickCount64();

        if (StartToolbarCompositionAnimation(toolbarRevealAnimationStartProgress_, targetProgress)) {
            const UINT completionDelayMs = targetProgress <= 0.001f && !IsZoomed(window_)
                ? kToolbarHideDetachDelayMs
                : kToolbarRevealDurationMs + 32;
            if (completionDelayMs == kToolbarHideDetachDelayMs && timeBeginPeriod(1) == TIMERR_NOERROR) {
                toolbarRevealTimerResolutionActive_ = true;
            }
            SetTimer(window_, kToolbarRevealTimerId, completionDelayMs, nullptr);
            return;
        }

        if (timeBeginPeriod(1) == TIMERR_NOERROR) {
            toolbarRevealTimerResolutionActive_ = true;
        }
        SetTimer(window_, kToolbarRevealTimerId, kToolbarRevealFrameMs, nullptr);
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    RECT toolbarRect = clientRect;
    toolbarRect.bottom = FullToolbarHeight();
    RedrawWindow(window_, &toolbarRect, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    static_cast<void>(UpdatePdfSidebarComposition(clientRect));
}

void App::StopToolbarRevealAnimationTimer() {
    if (window_ != nullptr) {
        KillTimer(window_, kToolbarRevealTimerId);
    }
    toolbarCompositionAnimationActive_ = false;
    if (toolbarRevealTimerResolutionActive_) {
        timeEndPeriod(1);
        toolbarRevealTimerResolutionActive_ = false;
    }
}

void App::HandleToolbarRevealAnimationTimer() {
    if (window_ == nullptr) {
        return;
    }

    if (toolbarCompositionAnimationActive_) {
        toolbarRevealProgress_ = toolbarRevealAnimationTargetProgress_;
        toolbarRevealAnimationStartProgress_ = toolbarRevealProgress_;
        toolbarRevealAnimationStartMs_ = 0;
        StopToolbarRevealAnimationTimer();
        RECT clientRect {};
        GetClientRect(window_, &clientRect);
        static_cast<void>(UpdateToolbarComposition(clientRect));
        static_cast<void>(UpdatePdfSidebarComposition(clientRect));
        if (toolbarRevealProgress_ <= 0.001f) {
            RECT toolbarRect = clientRect;
            toolbarRect.bottom = std::min<LONG>(
                clientRect.bottom,
                static_cast<LONG>(FullToolbarHeight() + ToolbarHideAnimationOvershoot(FullToolbarHeight())));
            RedrawWindow(window_, &toolbarRect, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
            DwmFlush();
        }
        return;
    }

    const ULONGLONG startMs = toolbarRevealAnimationStartMs_;
    const ULONGLONG nowMs = GetTickCount64();
    const double elapsed = startMs > 0 ? static_cast<double>(nowMs - startMs) : static_cast<double>(kToolbarRevealDurationMs);
    const float linearT = static_cast<float>(std::clamp(elapsed / static_cast<double>(kToolbarRevealDurationMs), 0.0, 1.0));
    const float easedT = linearT * linearT * (3.0f - (2.0f * linearT));
    toolbarRevealProgress_ = toolbarRevealAnimationStartProgress_
        + ((toolbarRevealAnimationTargetProgress_ - toolbarRevealAnimationStartProgress_) * easedT);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    RECT toolbarRect = clientRect;
    toolbarRect.bottom = FullToolbarHeight();
    RedrawWindow(window_, &toolbarRect, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    static_cast<void>(UpdatePdfSidebarComposition(clientRect));
    DwmFlush();

    if (linearT >= 1.0f) {
        toolbarRevealProgress_ = toolbarRevealAnimationTargetProgress_;
        toolbarRevealAnimationStartProgress_ = toolbarRevealProgress_;
        StopToolbarRevealAnimationTimer();
    }
}

void App::InvalidateCanvas() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    clientRect.top = ReservedToolbarHeight();
    InvalidateRect(window_, &clientRect, FALSE);
}

void App::InvalidateViewport() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT viewportRect = CanvasRect(clientRect);
    InvalidateRect(window_, &viewportRect, FALSE);
}

void App::InvalidatePageSurfaceCache(std::size_t pageIndex) {
    if (pageIndex >= pageSurfaceCaches_.size()) {
        return;
    }

    contentCompositionDirty_ = true;
    PageSurfaceCache& cache = pageSurfaceCaches_[pageIndex];
    cache.valid = false;
    cache.pixels.clear();
    cache.width = 0;
    cache.height = 0;
    cache.stride = 0;
    cache.imageCount = 0;
    cache.textCount = 0;
    cache.textSignature = 0;
    cache.rasterWidth = 0;
    cache.rasterHeight = 0;
    cache.rasterScale = 0.0f;
    cache.sourcePdfSourceIndex = -1;
    cache.sourcePdfPageIndex = -1;
    cache.pageScale = 0.0f;
    cache.strokeCount = 0;
    InvalidateSidebarThumbnailCache(pageIndex);
}

void App::InvalidateAllPageSurfaceCaches() {
    for (std::size_t pageIndex = 0; pageIndex < pageSurfaceCaches_.size(); ++pageIndex) {
        InvalidatePageSurfaceCache(pageIndex);
    }
}

void App::InvalidateVisiblePageSurfaceCaches() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const int visibleTop = ReservedToolbarHeight();
    const int visibleBottom = static_cast<int>(clientRect.bottom);
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }
        if (pageView.rect.bottom < visibleTop || pageView.rect.top > visibleBottom) {
            continue;
        }
        InvalidatePageSurfaceCache(pageView.pageIndex);
    }
}

void App::InvalidateStrokeRegion(const Stroke& stroke, std::size_t pageIndex, bool smoothLiveStroke) {
    if (window_ == nullptr || stroke.points.empty()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, pageIndex);
    if (!pageView.valid) {
        return;
    }

    const std::optional<RECT> strokeBounds = StrokeRenderBoundsRect(
        stroke,
        pageView.scale,
        smoothLiveStroke,
        settings_.strokeSmoothing,
        12);
    if (!strokeBounds.has_value()) {
        return;
    }

    RECT dirtyRect = *strokeBounds;
    OffsetRect(&dirtyRect, pageView.rect.left, pageView.rect.top);
    InvalidateRect(window_, &dirtyRect, FALSE);
}

void App::RefreshCursor() {
    if (window_ == nullptr) {
        return;
    }

    POINT cursor {};
    if (GetCursorPos(&cursor) == FALSE) {
        return;
    }

    ScreenToClient(window_, &cursor);
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    if (PtInRect(&clientRect, cursor) == 0) {
        return;
    }

    SendMessageW(window_, WM_SETCURSOR, reinterpret_cast<WPARAM>(window_), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

void App::SetActiveTool(Tool tool) {
    if (tool != Tool::Text) {
        CommitTextEdit();
    }
    if (tool == Tool::Select) {
        selectionMode_ = SelectionMode::Lasso;
    }
    if (tool != Tool::Select) {
        isSelecting_ = false;
        selectionLassoCursorPoints_.clear();
        selectionBoxOverride_ = false;
        UpdateCaptureState();
    }
    if (tool != Tool::Move) {
        ClearInteractionPreview();
        pendingTextEditClick_ = false;
        pendingTextEditPageIndex_.reset();
        pendingTextEditIndex_.reset();
        isMovingSelection_ = false;
        moveSelectionPageIndex_.reset();
        isTransformingImage_ = false;
        transformingImagePageIndex_.reset();
        transformingImageIndex_.reset();
        imageTransformHandle_ = ImageTransformHandle::None;
        isTransformingText_ = false;
        transformingTextPageIndex_.reset();
        transformingTextIndex_.reset();
        textTransformHandle_ = ImageTransformHandle::None;
        isTransformingStroke_ = false;
        transformingStrokePageIndex_.reset();
        strokeTransformHandle_ = ImageTransformHandle::None;
        strokeTransformInitials_.clear();
    }
    if (tool != activeTool_) {
        CancelQuickSizePopup();
        if (tool != Tool::Select && HasSelection()) {
            ClearSelection();
        }
    }
    activeTool_ = tool;
    SyncBrushWidthFromActiveTool();
    RefreshCursor();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::PreviousPage() {
    if (CanGoToPreviousPage()) {
        FocusPage(document_.CurrentPageIndex() - 1U);
    }
}

void App::NextPage() {
    if (CanGoToNextPage()) {
        FocusPage(document_.CurrentPageIndex() + 1U);
    }
}

void App::SetCurrentPageIndex(std::size_t index) {
    if (document_.SetCurrentPageIndex(index)) {
        targetZoom_ = zoom_;
        MarkPdfSidebarCompositionDirty();
        EnsureSidebarCurrentPageVisible();
        InvalidateRect(window_, nullptr, FALSE);
    }
}


std::optional<App::ImageTransformHandle> App::HitTestSelectedImageHandle(POINT cursor, std::size_t* pageIndex, std::size_t* imageIndex) const {
    const auto singleImage = SingleSelectedImage();
    if (!singleImage.has_value() || window_ == nullptr) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, singleImage->first);
    if (!pageView.valid) {
        return std::nullopt;
    }

    const Page& page = document_.PageAt(singleImage->first);
    if (singleImage->second >= page.images.size()) {
        return std::nullopt;
    }

    const PageImage& image = page.images[singleImage->second];
    const UINT dpi = WindowDpi(window_);
    const auto corners = ImageCorners(image);
    const Vec2 topMid {
        (corners[0].x + corners[1].x) * 0.5f,
        (corners[0].y + corners[1].y) * 0.5f,
    };
    const Vec2 bottomMid {
        (corners[2].x + corners[3].x) * 0.5f,
        (corners[2].y + corners[3].y) * 0.5f,
    };
    const Vec2 leftMid {
        (corners[0].x + corners[3].x) * 0.5f,
        (corners[0].y + corners[3].y) * 0.5f,
    };
    const Vec2 rightMid {
        (corners[1].x + corners[2].x) * 0.5f,
        (corners[1].y + corners[2].y) * 0.5f,
    };
    const Vec2 center = ImageCenter(image);
    const Vec2 rotateHandle = RotatePointAround({ center.x, image.y - SelectionRotateOffsetPageUnits(pageView.scale, dpi) }, center, image.rotationDegrees);

    const struct {
        ImageTransformHandle handle;
        Vec2 point;
    } handlePoints[] {
        { ImageTransformHandle::ResizeNorthWest, ScreenPointFromPagePoint(corners[0], pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeNorth, ScreenPointFromPagePoint(topMid, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeNorthEast, ScreenPointFromPagePoint(corners[1], pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeEast, ScreenPointFromPagePoint(rightMid, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouthEast, ScreenPointFromPagePoint(corners[2], pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouth, ScreenPointFromPagePoint(bottomMid, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouthWest, ScreenPointFromPagePoint(corners[3], pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeWest, ScreenPointFromPagePoint(leftMid, pageView.rect, pageView.scale) },
        { ImageTransformHandle::Rotate, ScreenPointFromPagePoint(rotateHandle, pageView.rect, pageView.scale) },
    };
    const float handleRadiusPixels = SelectionHandleHitRadiusPixels(dpi);
    const Vec2 cursorPoint { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };
    for (const auto& handlePoint : handlePoints) {
        if (DistanceSquared(handlePoint.point, cursorPoint) <= (handleRadiusPixels * handleRadiusPixels)) {
            if (pageIndex != nullptr) {
                *pageIndex = singleImage->first;
            }
            if (imageIndex != nullptr) {
                *imageIndex = singleImage->second;
            }
            return handlePoint.handle;
        }
    }

    return std::nullopt;
}

std::optional<std::pair<std::size_t, std::size_t>> App::HitTestText(POINT cursor) const {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value() || hitPage->pageIndex >= document_.PageCount()) {
        return std::nullopt;
    }

    const Vec2 pagePoint = ScreenToPage(*hitPage, cursor, true);
    const Page& page = document_.PageAt(hitPage->pageIndex);
    for (std::size_t textIndex = page.texts.size(); textIndex > 0; --textIndex) {
        const std::size_t actualIndex = textIndex - 1U;
        if (TextHitsPoint(page.texts[actualIndex], page.width, pagePoint)) {
            return std::make_pair(hitPage->pageIndex, actualIndex);
        }
    }

    return std::nullopt;
}

std::optional<App::ImageTransformHandle> App::HitTestSelectedTextHandle(POINT cursor, std::size_t* pageIndex, std::size_t* textIndex) const {
    const auto singleText = SingleSelectedText();
    if (!singleText.has_value() || window_ == nullptr) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, singleText->first);
    if (!pageView.valid) {
        return std::nullopt;
    }

    const Page& page = document_.PageAt(singleText->first);
    if (singleText->second >= page.texts.size()) {
        return std::nullopt;
    }

    const auto boundsRect = PageTextBoundsRect(page.texts[singleText->second], page.width, pageView.scale, 0);
    if (!boundsRect.has_value()) {
        return std::nullopt;
    }
    RECT screenBounds = *boundsRect;
    OffsetRect(&screenBounds, pageView.rect.left, pageView.rect.top);
    const Vec2 center {
        static_cast<float>((screenBounds.left + screenBounds.right) / 2),
        static_cast<float>((screenBounds.top + screenBounds.bottom) / 2),
    };
    const struct {
        ImageTransformHandle handle;
        Vec2 point;
    } handlePoints[] {
        { ImageTransformHandle::ResizeNorthWest, { static_cast<float>(screenBounds.left), static_cast<float>(screenBounds.top) } },
        { ImageTransformHandle::ResizeNorth, { center.x, static_cast<float>(screenBounds.top) } },
        { ImageTransformHandle::ResizeNorthEast, { static_cast<float>(screenBounds.right), static_cast<float>(screenBounds.top) } },
        { ImageTransformHandle::ResizeEast, { static_cast<float>(screenBounds.right), center.y } },
        { ImageTransformHandle::ResizeSouthEast, { static_cast<float>(screenBounds.right), static_cast<float>(screenBounds.bottom) } },
        { ImageTransformHandle::ResizeSouth, { center.x, static_cast<float>(screenBounds.bottom) } },
        { ImageTransformHandle::ResizeSouthWest, { static_cast<float>(screenBounds.left), static_cast<float>(screenBounds.bottom) } },
        { ImageTransformHandle::ResizeWest, { static_cast<float>(screenBounds.left), center.y } },
    };
    const float handleRadiusPixels = SelectionHandleHitRadiusPixels(WindowDpi(window_));
    const Vec2 cursorPoint { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };
    for (const auto& handlePoint : handlePoints) {
        if (DistanceSquared(handlePoint.point, cursorPoint) <= (handleRadiusPixels * handleRadiusPixels)) {
            if (pageIndex != nullptr) {
                *pageIndex = singleText->first;
            }
            if (textIndex != nullptr) {
                *textIndex = singleText->second;
            }
            return handlePoint.handle;
        }
    }

    return std::nullopt;
}

std::optional<App::ImageTransformHandle> App::HitTestSelectedStrokeHandle(POINT cursor, std::size_t* pageIndex) const {
    const auto strokePage = SingleSelectedStrokePage();
    if (!strokePage.has_value() || window_ == nullptr || *strokePage >= selectedStrokeIndices_.size() || selectedStrokeIndices_[*strokePage].empty()) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *strokePage);
    if (!pageView.valid) {
        return std::nullopt;
    }

    const Page& page = document_.PageAt(*strokePage);
    const UINT dpi = WindowDpi(window_);
    float minX = page.width;
    float minY = page.height;
    float maxX = 0.0f;
    float maxY = 0.0f;
    for (std::size_t strokeIndex : selectedStrokeIndices_[*strokePage]) {
        if (strokeIndex >= page.strokes.size()) {
            continue;
        }
        for (const Vec2& point : page.strokes[strokeIndex].points) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
    }

    const Vec2 center { (minX + maxX) * 0.5f, (minY + maxY) * 0.5f };
    const struct {
        ImageTransformHandle handle;
        Vec2 point;
    } handlePoints[] {
        { ImageTransformHandle::ResizeNorthWest, ScreenPointFromPagePoint({ minX, minY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeNorth, ScreenPointFromPagePoint({ center.x, minY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeNorthEast, ScreenPointFromPagePoint({ maxX, minY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeEast, ScreenPointFromPagePoint({ maxX, center.y }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouthEast, ScreenPointFromPagePoint({ maxX, maxY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouth, ScreenPointFromPagePoint({ center.x, maxY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouthWest, ScreenPointFromPagePoint({ minX, maxY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeWest, ScreenPointFromPagePoint({ minX, center.y }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::Rotate, ScreenPointFromPagePoint({ center.x, minY - SelectionRotateOffsetPageUnits(pageView.scale, dpi) }, pageView.rect, pageView.scale) },
    };
    const float handleRadiusPixels = SelectionHandleHitRadiusPixels(dpi);
    const Vec2 cursorPoint { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };
    for (const auto& handlePoint : handlePoints) {
        if (DistanceSquared(handlePoint.point, cursorPoint) <= (handleRadiusPixels * handleRadiusPixels)) {
            if (pageIndex != nullptr) {
                *pageIndex = *strokePage;
            }
            return handlePoint.handle;
        }
    }

    return std::nullopt;
}

std::vector<Vec2> App::CurrentSelectionLassoPolygon() const {
    std::vector<Vec2> polygon;
    polygon.reserve(selectionLassoCursorPoints_.size() + 1U);
    for (POINT point : selectionLassoCursorPoints_) {
        polygon.push_back(PointToVec2(point));
    }
    if (polygon.empty()) {
        return polygon;
    }

    if (DistanceSquared(polygon.back(), PointToVec2(selectionCurrentCursor_)) > 0.0001f) {
        polygon.push_back(PointToVec2(selectionCurrentCursor_));
    }
    return polygon;
}

bool App::CursorHitsSelectedContent(POINT cursor) const {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        return false;
    }

    const std::size_t pageIndex = hitPage->pageIndex;
    if (pageIndex >= selectedStrokeIndices_.size()
        && pageIndex >= selectedTextIndices_.size()
        && pageIndex >= selectedImageIndices_.size()) {
        return false;
    }

    const Page& page = document_.PageAt(pageIndex);
    const float pageX = (static_cast<float>(cursor.x - hitPage->rect.left)) / hitPage->scale;
    const float pageY = (static_cast<float>(cursor.y - hitPage->rect.top)) / hitPage->scale;
    const Vec2 pagePoint {
        std::clamp(pageX, 0.0f, page.width),
        std::clamp(pageY, 0.0f, page.height),
    };
    const float hitRadius = std::max(8.0f, brushWidth_ * 2.5f) / std::max(0.1f, hitPage->scale);

    if (pageIndex < selectedStrokeIndices_.size()) {
        for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
            if (strokeIndex < page.strokes.size() && StrokeHitsPoint(page.strokes[strokeIndex], pagePoint, hitRadius, settings_.strokeSmoothing)) {
                return true;
            }
        }
    }

    if (pageIndex < selectedTextIndices_.size()) {
        for (std::size_t textIndex : selectedTextIndices_[pageIndex]) {
            if (textIndex < page.texts.size() && TextHitsPoint(page.texts[textIndex], page.width, pagePoint)) {
                return true;
            }
        }
    }

    if (pageIndex < selectedImageIndices_.size()) {
        for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
            if (imageIndex < page.images.size() && ImageHitsPoint(page.images[imageIndex], pagePoint)) {
                return true;
            }
        }
    }

    return false;
}

bool App::CursorHitsSelectionMoveGrip(POINT cursor) const {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        return false;
    }

    const std::size_t pageIndex = hitPage->pageIndex;
    if (pageIndex >= document_.PageCount()) {
        return false;
    }

    const bool hasSelectedStrokes = pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty();
    const bool hasSelectedTexts = pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[pageIndex].empty();
    const bool hasSelectedImages = pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty();
    if (!hasSelectedStrokes && !hasSelectedTexts && !hasSelectedImages) {
        return false;
    }

    const Page& page = document_.PageAt(pageIndex);
    const UINT dpi = window_ != nullptr ? WindowDpi(window_) : SystemDpi();
    const int gripPadding = std::max(2, ScaleForDpi(2, dpi));

    if (hasSelectedImages) {
        for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
            if (imageIndex >= page.images.size()) {
                continue;
            }

            std::vector<Vec2> screenCorners;
            screenCorners.reserve(4U);
            for (const Vec2& corner : ImageCorners(page.images[imageIndex])) {
                screenCorners.push_back(ScreenPointFromPagePoint(corner, hitPage->rect, hitPage->scale));
            }
            if (PolygonContainsPoint(screenCorners, cursor)) {
                return true;
            }
        }
    }

    if (hasSelectedTexts) {
        for (std::size_t textIndex : selectedTextIndices_[pageIndex]) {
            if (textIndex >= page.texts.size()) {
                continue;
            }

            const auto textRect = PageTextBoundsRect(page.texts[textIndex], page.width, hitPage->scale, 4);
            if (!textRect.has_value()) {
                continue;
            }

            RECT screenRect = *textRect;
            OffsetRect(&screenRect, hitPage->rect.left, hitPage->rect.top);
            if (RectContainsPointWithPadding(screenRect, cursor, gripPadding)) {
                return true;
            }
        }
    }

    if (hasSelectedStrokes) {
        float minX = page.width;
        float minY = page.height;
        float maxX = 0.0f;
        float maxY = 0.0f;
        bool hasStrokeBounds = false;
        for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
            if (strokeIndex >= page.strokes.size()) {
                continue;
            }
            for (const Vec2& point : page.strokes[strokeIndex].points) {
                minX = std::min(minX, point.x);
                minY = std::min(minY, point.y);
                maxX = std::max(maxX, point.x);
                maxY = std::max(maxY, point.y);
                hasStrokeBounds = true;
            }
        }

        if (hasStrokeBounds) {
            const RECT screenRect = MakeRect(
                hitPage->rect.left + static_cast<int>(std::lround(minX * hitPage->scale)),
                hitPage->rect.top + static_cast<int>(std::lround(minY * hitPage->scale)),
                hitPage->rect.left + static_cast<int>(std::lround(maxX * hitPage->scale)),
                hitPage->rect.top + static_cast<int>(std::lround(maxY * hitPage->scale)));
            if (RectContainsPointWithPadding(screenRect, cursor, gripPadding)) {
                return true;
            }
        }
    }

    return false;
}

bool App::CursorInteractsWithSelection(POINT cursor) const {
    return HitTestSelectedStrokeHandle(cursor).has_value()
        || HitTestSelectedTextHandle(cursor).has_value()
        || HitTestSelectedImageHandle(cursor).has_value()
        || CursorHitsSelectionMoveGrip(cursor)
        || CursorHitsSelectedContent(cursor);
}

void App::TranslateSelectedContent(std::size_t pageIndex, Vec2 delta, bool clampToPage, bool notify) {
    if (pageIndex >= document_.PageCount()) {
        return;
    }

    Page& page = document_.PageAt(pageIndex);
    if (pageIndex >= selectedStrokeIndices_.size()
        || pageIndex >= selectedTextIndices_.size()
        || pageIndex >= selectedImageIndices_.size()) {
        return;
    }

    float minX = page.width;
    float minY = page.height;
    float maxX = 0.0f;
    float maxY = 0.0f;
    bool hasSelection = false;

    for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
        if (strokeIndex >= page.strokes.size()) {
            continue;
        }
        for (const Vec2& point : page.strokes[strokeIndex].points) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
            hasSelection = true;
        }
    }

    for (std::size_t textIndex : selectedTextIndices_[pageIndex]) {
        if (textIndex >= page.texts.size()) {
            continue;
        }
        const PageTextBounds bounds = MeasurePageTextBounds(page.texts[textIndex], page.width);
        minX = std::min(minX, bounds.left);
        minY = std::min(minY, bounds.top);
        maxX = std::max(maxX, bounds.right);
        maxY = std::max(maxY, bounds.bottom);
        hasSelection = true;
    }

    for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
        if (imageIndex >= page.images.size()) {
            continue;
        }
        const PageImage& image = page.images[imageIndex];
        const auto corners = ImageCorners(image);
        for (const Vec2& corner : corners) {
            minX = std::min(minX, corner.x);
            minY = std::min(minY, corner.y);
            maxX = std::max(maxX, corner.x);
            maxY = std::max(maxY, corner.y);
        }
        hasSelection = true;
    }

    if (!hasSelection) {
        return;
    }

    if (clampToPage) {
        delta.x = std::clamp(delta.x, -minX, page.width - maxX);
        delta.y = std::clamp(delta.y, -minY, page.height - maxY);
    }
    if (std::abs(delta.x) < 0.001f && std::abs(delta.y) < 0.001f) {
        return;
    }

    for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
        if (strokeIndex >= page.strokes.size()) {
            continue;
        }
        for (Vec2& point : page.strokes[strokeIndex].points) {
            point.x += delta.x;
            point.y += delta.y;
        }
    }

    for (std::size_t textIndex : selectedTextIndices_[pageIndex]) {
        if (textIndex >= page.texts.size()) {
            continue;
        }
        page.texts[textIndex].x += delta.x;
        page.texts[textIndex].y += delta.y;
    }

    for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
        if (imageIndex >= page.images.size()) {
            continue;
        }
        page.images[imageIndex].x += delta.x;
        page.images[imageIndex].y += delta.y;
    }

    InvalidatePageSurfaceCache(pageIndex);
    if (notify) {
        NotifyDocumentMutated();
        InvalidateCanvas();
    }
}

bool App::RehomeSelectedContentAcrossPages(bool clampToPage) {
    if (window_ == nullptr || document_.PageCount() == 0U) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const std::vector<PageView> pageViews = CalculatePageViews(clientRect);
    if (pageViews.size() != document_.PageCount()) {
        return false;
    }

    const auto targetPageForScreenPoint = [&](POINT screenPoint, std::size_t fallbackPageIndex) {
        std::size_t bestPageIndex = std::min(fallbackPageIndex, pageViews.size() - 1U);
        long bestDistanceSquared = LONG_MAX;
        for (const PageView& pageView : pageViews) {
            if (!pageView.valid) {
                continue;
            }
            if (PtInRect(&pageView.rect, screenPoint) != 0) {
                return pageView.pageIndex;
            }

            const long dx = static_cast<long>(std::max<LONG>(pageView.rect.left - screenPoint.x, 0))
                + static_cast<long>(std::max<LONG>(screenPoint.x - pageView.rect.right, 0));
            const long dy = static_cast<long>(std::max<LONG>(pageView.rect.top - screenPoint.y, 0))
                + static_cast<long>(std::max<LONG>(screenPoint.y - pageView.rect.bottom, 0));
            const long distanceSquared = (dx * dx) + (dy * dy);
            if (distanceSquared < bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                bestPageIndex = pageView.pageIndex;
            }
        }
        return bestPageIndex;
    };

    bool changed = false;
    std::vector<bool> invalidatedPages(document_.PageCount(), false);
    std::vector<std::vector<Stroke>> movedStrokes(document_.PageCount());
    std::vector<std::vector<PageImage>> movedImages(document_.PageCount());

    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        Page& page = document_.PageAt(pageIndex);
        const PageView& sourceView = pageViews[pageIndex];

        if (pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty()) {
            std::vector<bool> selectedFlags(page.strokes.size(), false);
            for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
                if (strokeIndex < selectedFlags.size()) {
                    selectedFlags[strokeIndex] = true;
                }
            }

            std::vector<Stroke> retainedStrokes;
            retainedStrokes.reserve(page.strokes.size());
            std::vector<std::size_t> retainedSelected;
            for (std::size_t strokeIndex = 0; strokeIndex < page.strokes.size(); ++strokeIndex) {
                Stroke stroke = std::move(page.strokes[strokeIndex]);
                if (!selectedFlags[strokeIndex]) {
                    retainedStrokes.push_back(std::move(stroke));
                    continue;
                }

                float minX = 0.0f;
                float minY = 0.0f;
                float maxX = 0.0f;
                float maxY = 0.0f;
                if (!StrokeBounds(stroke, &minX, &minY, &maxX, &maxY)) {
                    retainedStrokes.push_back(std::move(stroke));
                    retainedSelected.push_back(retainedStrokes.size() - 1U);
                    continue;
                }

                const POINT centerPoint {
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.left) + ((minX + maxX) * 0.5f * sourceView.scale))),
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.top) + ((minY + maxY) * 0.5f * sourceView.scale))),
                };
                const std::size_t targetPageIndex = targetPageForScreenPoint(centerPoint, pageIndex);
                if (targetPageIndex != pageIndex) {
                    const PageView& targetView = pageViews[targetPageIndex];
                    for (Vec2& point : stroke.points) {
                        point = RemapPagePointBetweenViews(sourceView, targetView, point);
                    }
                    if (clampToPage) {
                        TranslateStrokePoints(stroke, ClampStrokeDeltaToPage(stroke, document_.PageAt(targetPageIndex), {}));
                    }
                    movedStrokes[targetPageIndex].push_back(std::move(stroke));
                    invalidatedPages[pageIndex] = true;
                    invalidatedPages[targetPageIndex] = true;
                    changed = true;
                    continue;
                }

                if (clampToPage) {
                    const Vec2 clampDelta = ClampStrokeDeltaToPage(stroke, page, {});
                    if (std::abs(clampDelta.x) >= 0.001f || std::abs(clampDelta.y) >= 0.001f) {
                        TranslateStrokePoints(stroke, clampDelta);
                        invalidatedPages[pageIndex] = true;
                        changed = true;
                    }
                }

                retainedStrokes.push_back(std::move(stroke));
                retainedSelected.push_back(retainedStrokes.size() - 1U);
            }

            page.strokes = std::move(retainedStrokes);
            selectedStrokeIndices_[pageIndex] = std::move(retainedSelected);
        }

        if (pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty()) {
            std::vector<bool> selectedFlags(page.images.size(), false);
            for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
                if (imageIndex < selectedFlags.size()) {
                    selectedFlags[imageIndex] = true;
                }
            }

            std::vector<PageImage> retainedImages;
            retainedImages.reserve(page.images.size());
            std::vector<std::size_t> retainedSelected;
            for (std::size_t imageIndex = 0; imageIndex < page.images.size(); ++imageIndex) {
                PageImage image = std::move(page.images[imageIndex]);
                if (!selectedFlags[imageIndex]) {
                    retainedImages.push_back(std::move(image));
                    continue;
                }

                const Vec2 center = ImageCenter(image);
                const POINT centerPoint {
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.left) + (center.x * sourceView.scale))),
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.top) + (center.y * sourceView.scale))),
                };
                const std::size_t targetPageIndex = targetPageForScreenPoint(centerPoint, pageIndex);
                if (targetPageIndex != pageIndex) {
                    const PageView& targetView = pageViews[targetPageIndex];
                    const Vec2 remappedTopLeft = RemapPagePointBetweenViews(sourceView, targetView, { image.x, image.y });
                    image.x = remappedTopLeft.x;
                    image.y = remappedTopLeft.y;
                    if (clampToPage) {
                        const Vec2 clampDelta = ClampImageDeltaToPage(image, document_.PageAt(targetPageIndex), {});
                        image.x += clampDelta.x;
                        image.y += clampDelta.y;
                    }
                    movedImages[targetPageIndex].push_back(std::move(image));
                    invalidatedPages[pageIndex] = true;
                    invalidatedPages[targetPageIndex] = true;
                    changed = true;
                    continue;
                }

                if (clampToPage) {
                    const Vec2 clampDelta = ClampImageDeltaToPage(image, page, {});
                    if (std::abs(clampDelta.x) >= 0.001f || std::abs(clampDelta.y) >= 0.001f) {
                        image.x += clampDelta.x;
                        image.y += clampDelta.y;
                        invalidatedPages[pageIndex] = true;
                        changed = true;
                    }
                }

                retainedImages.push_back(std::move(image));
                retainedSelected.push_back(retainedImages.size() - 1U);
            }

            page.images = std::move(retainedImages);
            selectedImageIndices_[pageIndex] = std::move(retainedSelected);
        }
    }

    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (!movedStrokes[pageIndex].empty()) {
            auto& strokes = document_.PageAt(pageIndex).strokes;
            for (Stroke& stroke : movedStrokes[pageIndex]) {
                strokes.push_back(std::move(stroke));
                selectedStrokeIndices_[pageIndex].push_back(strokes.size() - 1U);
            }
            invalidatedPages[pageIndex] = true;
        }

        if (!movedImages[pageIndex].empty()) {
            auto& images = document_.PageAt(pageIndex).images;
            for (PageImage& image : movedImages[pageIndex]) {
                images.push_back(std::move(image));
                selectedImageIndices_[pageIndex].push_back(images.size() - 1U);
            }
            invalidatedPages[pageIndex] = true;
        }
    }

    for (std::size_t pageIndex = 0; pageIndex < invalidatedPages.size(); ++pageIndex) {
        if (invalidatedPages[pageIndex]) {
            InvalidatePageSurfaceCache(pageIndex);
        }
    }

    return changed;
}

std::wstring App::CurrentWindowTitle() const {
    std::wstring title;
    if (!document_.FilePath().empty()) {
        title = BaseName(document_.FilePath());
    } else if (!document_.SourcePath().empty()) {
        title = BaseName(document_.SourcePath());
    } else {
        title = L"Untitled";
    }

    if (documentDirty_) {
        title += L"*";
    }

    return title;
}

std::wstring App::ZoomIndicatorLabel() const {
    if (toolbarZoomEditing_) {
        return toolbarZoomText_.empty() ? L"" : toolbarZoomText_;
    }
    return FormatZoomPercentValue(zoom_) + L"%";
}

std::wstring App::PageIndicatorLabel() const {
    if (pageJumpEditing_) {
        return pageJumpText_.empty() ? L"" : pageJumpText_;
    }
    wchar_t buffer[64] = {};
    std::swprintf(buffer, std::size(buffer), L"%zu / %zu", document_.CurrentPageIndex() + 1U, document_.PageCount());
    return buffer;
}

bool App::IsPdfSidebarVisible() const {
    return document_.PageCount() > 0U && (pdfSidebarOpen_ || pdfSidebarSlideProgress_ > 0.001f);
}

void App::MarkPdfSidebarCompositionDirty() {
    pdfSidebarCompositionDirty_ = true;
}

float App::PdfSidebarOffsetXForProgress(int sidebarWidth, UINT dpi, float progress) const {
    const int horizontalInset = IsZoomed(window_) ? WindowFrameThicknessX(dpi) : 0;
    progress = std::clamp(progress, 0.0f, 1.0f);
    return static_cast<float>(horizontalInset) - (static_cast<float>(sidebarWidth) * (1.0f - progress));
}

bool App::EnsureCompositionDevice() {
    if (compositionDevice_ != nullptr
        && compositionTarget_ != nullptr
        && compositionRootVisual_ != nullptr
        && contentCompositionVisual_ != nullptr
        && toolbarViewportCompositionVisual_ != nullptr
        && toolbarCompositionVisual_ != nullptr
        && pdfSidebarCompositionVisual_ != nullptr
        && popupCompositionVisual_ != nullptr) {
        return true;
    }
    if (window_ == nullptr) {
        return false;
    }

    ReleaseCompositionResources();

    ID3D11DeviceContext* d3dContext = nullptr;
    D3D_FEATURE_LEVEL featureLevel {};
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &compositionD3dDevice_,
        &featureLevel,
        &d3dContext);
    ReleaseCom(d3dContext);
    if (FAILED(hr) || compositionD3dDevice_ == nullptr) {
        ReleaseCompositionResources();
        return false;
    }

    hr = compositionD3dDevice_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&compositionDxgiDevice_));
    if (FAILED(hr) || compositionDxgiDevice_ == nullptr) {
        ReleaseCompositionResources();
        return false;
    }

    hr = DCompositionCreateDevice(
        compositionDxgiDevice_,
        __uuidof(IDCompositionDevice),
        reinterpret_cast<void**>(&compositionDevice_));
    if (FAILED(hr) || compositionDevice_ == nullptr) {
        ReleaseCompositionResources();
        return false;
    }

    hr = compositionDevice_->CreateTargetForHwnd(window_, TRUE, &compositionTarget_);
    if (FAILED(hr) || compositionTarget_ == nullptr) {
        ReleaseCompositionResources();
        return false;
    }

    hr = compositionDevice_->CreateVisual(&compositionRootVisual_);
    if (FAILED(hr) || compositionRootVisual_ == nullptr) {
        ReleaseCompositionResources();
        return false;
    }

    hr = compositionDevice_->CreateVisual(&contentViewportCompositionVisual_);
    if (FAILED(hr) || contentViewportCompositionVisual_ == nullptr) {
        ReleaseCompositionResources();
        return false;
    }

    hr = compositionDevice_->CreateVisual(&contentCompositionVisual_);
    if (FAILED(hr) || contentCompositionVisual_ == nullptr) {
        ReleaseCompositionResources();
        return false;
    }

    hr = compositionDevice_->CreateRectangleClip(&contentViewportClip_);
    if (FAILED(hr) || contentViewportClip_ == nullptr) {
        ReleaseCompositionResources();
        return false;
    }

    hr = compositionDevice_->CreateVisual(&pdfSidebarCompositionVisual_);
    if (FAILED(hr) || pdfSidebarCompositionVisual_ == nullptr) {
        ReleaseCompositionResources();
        return false;
    }

    hr = compositionDevice_->CreateVisual(&toolbarViewportCompositionVisual_);
    if (FAILED(hr) || toolbarViewportCompositionVisual_ == nullptr) {
        ReleaseCompositionResources();
        return false;
    }

    hr = compositionDevice_->CreateVisual(&toolbarCompositionVisual_);
    if (FAILED(hr) || toolbarCompositionVisual_ == nullptr) {
        ReleaseCompositionResources();
        return false;
    }

    hr = compositionDevice_->CreateVisual(&popupCompositionVisual_);
    if (FAILED(hr) || popupCompositionVisual_ == nullptr) {
        ReleaseCompositionResources();
        return false;
    }

    contentViewportCompositionVisual_->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
    contentViewportCompositionVisual_->SetClip(contentViewportClip_);
    contentCompositionVisual_->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
    pdfSidebarCompositionVisual_->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
    toolbarViewportCompositionVisual_->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
    toolbarCompositionVisual_->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
    popupCompositionVisual_->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
    contentViewportCompositionVisual_->AddVisual(contentCompositionVisual_, TRUE, nullptr);
    toolbarViewportCompositionVisual_->AddVisual(toolbarCompositionVisual_, TRUE, nullptr);
    compositionRootVisual_->AddVisual(contentViewportCompositionVisual_, TRUE, nullptr);
    compositionRootVisual_->AddVisual(pdfSidebarCompositionVisual_, TRUE, contentViewportCompositionVisual_);
    compositionRootVisual_->AddVisual(toolbarViewportCompositionVisual_, TRUE, pdfSidebarCompositionVisual_);
    compositionRootVisual_->AddVisual(popupCompositionVisual_, TRUE, toolbarViewportCompositionVisual_);
    compositionTarget_->SetRoot(compositionRootVisual_);
    compositionDevice_->Commit();
    pdfSidebarCompositionDirty_ = true;
    return true;
}

void App::ReleaseCompositionResources() {
    if (compositionDevice_ != nullptr && pdfSidebarCompositionVisual_ != nullptr) {
        if (contentCompositionVisual_ != nullptr) {
            contentCompositionVisual_->SetContent(nullptr);
        }
        if (contentViewportCompositionVisual_ != nullptr) {
            contentViewportCompositionVisual_->SetClip(nullptr);
        }
        pdfSidebarCompositionVisual_->SetContent(nullptr);
        if (toolbarCompositionVisual_ != nullptr) {
            toolbarCompositionVisual_->SetContent(nullptr);
        }
        if (popupCompositionVisual_ != nullptr) {
            popupCompositionVisual_->SetContent(nullptr);
        }
        compositionDevice_->Commit();
    }
    ReleaseCom(contentCompositionSurface_);
    ReleaseCom(toolbarCompositionSurface_);
    ReleaseCom(pdfSidebarCompositionSurface_);
    ReleaseCom(popupCompositionSurface_);
    ReleaseCom(popupCompositionVisual_);
    ReleaseCom(toolbarCompositionVisual_);
    ReleaseCom(toolbarViewportCompositionVisual_);
    ReleaseCom(pdfSidebarCompositionVisual_);
    ReleaseCom(contentCompositionVisual_);
    ReleaseCom(contentViewportClip_);
    ReleaseCom(contentViewportCompositionVisual_);
    ReleaseCom(compositionRootVisual_);
    ReleaseCom(compositionTarget_);
    ReleaseCom(compositionDevice_);
    ReleaseCom(compositionDxgiDevice_);
    ReleaseCom(compositionD3dDevice_);
    pdfSidebarCompositionDirty_ = true;
    pdfSidebarCompositionAnimationActive_ = false;
    contentCompositionContentAttached_ = false;
    contentCompositionDirty_ = true;
    contentCompositionWidth_ = 0;
    contentCompositionHeight_ = 0;
    contentCompositionDpi_ = 0;
    contentCompositionPaddingX_ = 0;
    contentCompositionPaddingY_ = 0;
    popupCompositionContentAttached_ = false;
    popupCompositionWidth_ = 0;
    popupCompositionHeight_ = 0;
    popupCompositionDpi_ = 0;
    pdfSidebarCompositionWidth_ = 0;
    pdfSidebarCompositionHeight_ = 0;
    pdfSidebarCompositionDpi_ = 0;
    toolbarCompositionDirty_ = true;
    toolbarCompositionAnimationActive_ = false;
    toolbarCompositionContentAttached_ = false;
    toolbarCompositionSignature_ = 0;
    toolbarCompositionWidth_ = 0;
    toolbarCompositionHeight_ = 0;
    toolbarCompositionDpi_ = 0;
    toolbarCompositionMaximized_ = false;
}

void App::DetachContentComposition(bool waitForCommitCompletion) {
    if (compositionDevice_ == nullptr || contentCompositionVisual_ == nullptr || !contentCompositionContentAttached_) {
        return;
    }

    contentCompositionVisual_->SetContent(nullptr);
    compositionDevice_->Commit();
    if (waitForCommitCompletion) {
        compositionDevice_->WaitForCommitCompletion();
    }
    contentCompositionContentAttached_ = false;
    contentCompositionDirty_ = true;
}

bool App::UpdateContentComposition(RECT clientRect, RECT dirtyRect) {
    if (textEditWindow_ != nullptr && IsWindowVisible(textEditWindow_) != FALSE) {
        return false;
    }
    if (textFormatPopupOpen_) {
        return false;
    }
    if (!EnsureCompositionDevice()) {
        return false;
    }

    const UINT dpi = WindowDpi(window_);
    const int width = std::max(1, static_cast<int>(clientRect.right - clientRect.left));
    const int height = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top));
    const bool useSmoothScrollPadding = smoothScrollActive_;
    const int paddingX = useSmoothScrollPadding ? std::max(kSmoothScrollPaddingFloor, static_cast<int>(std::lround(static_cast<float>(width) * kSmoothScrollPaddingRatio))) : 0;
    const int paddingY = useSmoothScrollPadding ? std::max(kSmoothScrollPaddingFloor, static_cast<int>(std::lround(static_cast<float>(height) * kSmoothScrollPaddingRatio))) : 0;
    const int surfaceWidth = width + (paddingX * 2);
    const int surfaceHeight = height + (paddingY * 2);
    const RECT surfaceRect { 0, 0, surfaceWidth, surfaceHeight };
    RECT updateRect {};
    if (useSmoothScrollPadding) {
        updateRect = surfaceRect;
    } else if (IntersectRect(&updateRect, &dirtyRect, &surfaceRect) == 0) {
        return true;
    }

    bool createdSurface = false;
    if (contentCompositionSurface_ == nullptr
        || contentCompositionWidth_ != surfaceWidth
        || contentCompositionHeight_ != surfaceHeight
        || contentCompositionDpi_ != dpi
        || contentCompositionPaddingX_ != paddingX
        || contentCompositionPaddingY_ != paddingY) {
        ReleaseCom(contentCompositionSurface_);
        contentCompositionContentAttached_ = false;
        contentCompositionDirty_ = true;
        const HRESULT hr = compositionDevice_->CreateSurface(
            static_cast<UINT>(surfaceWidth),
            static_cast<UINT>(surfaceHeight),
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_IGNORE,
            &contentCompositionSurface_);
        if (FAILED(hr) || contentCompositionSurface_ == nullptr) {
            ReleaseCom(contentCompositionSurface_);
            return false;
        }
        contentCompositionWidth_ = surfaceWidth;
        contentCompositionHeight_ = surfaceHeight;
        contentCompositionDpi_ = dpi;
        contentCompositionPaddingX_ = paddingX;
        contentCompositionPaddingY_ = paddingY;
        createdSurface = true;
    }
    if (createdSurface || contentCompositionDirty_) {
        updateRect = surfaceRect;
    } else {
        OffsetRect(&updateRect, paddingX, paddingY);
    }

    IDXGISurface* dxgiSurface = nullptr;
    POINT updateOffset {};
    HRESULT hr = contentCompositionSurface_->BeginDraw(
        &updateRect,
        __uuidof(IDXGISurface),
        reinterpret_cast<void**>(&dxgiSurface),
        &updateOffset);
    if (FAILED(hr) || dxgiSurface == nullptr) {
        ReleaseCom(dxgiSurface);
        return false;
    }

    IDXGISurface1* gdiSurface = nullptr;
    hr = dxgiSurface->QueryInterface(__uuidof(IDXGISurface1), reinterpret_cast<void**>(&gdiSurface));
    ReleaseCom(dxgiSurface);
    if (FAILED(hr) || gdiSurface == nullptr) {
        contentCompositionSurface_->EndDraw();
        ReleaseCom(gdiSurface);
        return false;
    }

    HDC contentDc = nullptr;
    hr = gdiSurface->GetDC(FALSE, &contentDc);
    if (FAILED(hr) || contentDc == nullptr) {
        contentCompositionSurface_->EndDraw();
        ReleaseCom(gdiSurface);
        return false;
    }

    const int savedDc = SaveDC(contentDc);
    SetViewportOrgEx(contentDc, updateOffset.x - updateRect.left + paddingX, updateOffset.y - updateRect.top + paddingY, nullptr);
    IntersectClipRect(contentDc, updateRect.left, updateRect.top, updateRect.right, updateRect.bottom);
    HBRUSH backgroundBrush = CreateSolidBrush(RGB(18, 22, 29));
    FillRect(contentDc, &updateRect, backgroundBrush);
    DeleteObject(backgroundBrush);
    PaintCanvas(contentDc, clientRect);
    if (savedDc != 0) {
        RestoreDC(contentDc, savedDc);
    }

    gdiSurface->ReleaseDC(nullptr);
    ReleaseCom(gdiSurface);
    contentCompositionSurface_->EndDraw();

    if (contentViewportClip_ != nullptr) {
        contentViewportClip_->SetLeft(0.0f);
        contentViewportClip_->SetTop(0.0f);
        contentViewportClip_->SetRight(static_cast<float>(width));
        contentViewportClip_->SetBottom(static_cast<float>(height));
    }
    if (contentViewportCompositionVisual_ != nullptr) {
        contentViewportCompositionVisual_->SetOffsetX(0.0f);
        contentViewportCompositionVisual_->SetOffsetY(0.0f);
    }
    if (!smoothScrollActive_ || smoothScrollAnimationStartMs_ == 0) {
        contentCompositionVisual_->SetOffsetX(static_cast<float>(-paddingX) + CurrentSmoothScrollOffsetX());
        contentCompositionVisual_->SetOffsetY(static_cast<float>(-paddingY) + CurrentSmoothScrollOffsetY());
    }
    contentCompositionVisual_->SetContent(contentCompositionSurface_);
    contentCompositionContentAttached_ = true;
    contentCompositionDirty_ = false;
    compositionDevice_->Commit();
    return true;
}

void App::DetachPopupComposition(bool waitForCommitCompletion) {
    if (compositionDevice_ == nullptr || popupCompositionVisual_ == nullptr || !popupCompositionContentAttached_) {
        return;
    }

    popupCompositionVisual_->SetContent(nullptr);
    compositionDevice_->Commit();
    if (waitForCommitCompletion) {
        compositionDevice_->WaitForCommitCompletion();
    }
    popupCompositionContentAttached_ = false;
}

bool App::UpdatePopupComposition(RECT clientRect) {
    std::optional<RECT> overlayBounds;
    const UINT dpi = WindowDpi(window_);
    const auto includeBounds = [&](RECT rect) {
        RECT clipped {};
        if (IntersectRect(&clipped, &rect, &clientRect) == 0) {
            return;
        }
        overlayBounds = overlayBounds.has_value() ? UnionRectangles(*overlayBounds, clipped) : clipped;
    };

    if (quickSizePopupTool_.has_value()) {
        const RECT canvasRect = CanvasRect(clientRect);
        const Tool popupTool = *quickSizePopupTool_;
        const QuickSizePopupLayout layout = BuildQuickSizePopupLayout(
            canvasRect,
            quickSizePopupAnchor_,
            dpi,
            QuickSizePreviewExtentForTool(popupTool, clientRect));
        includeBounds(QuickSizePopupBounds(layout, dpi));
    }
    if (wheel_.paletteMenuOpen) {
        const ToolbarLayout toolbarLayout = BuildToolbarLayout(clientRect, palette_.size(), IsZoomed(window_), activeTool_, dpi);
        const PaletteMenuLayout layout = BuildPaletteMenuLayout(toolbarLayout, palette_.size(), dpi);
        includeBounds(InflateRectCopy(layout.panelRect, ScaleForDpi(3, dpi)));
    }
    if (wheel_.paletteWheelOpen) {
        const std::size_t wheelEntryCount = palette_.size() + wheel_.tools.size();
        if (wheelEntryCount > 0U) {
            const PaletteWheelLayout layout = BuildPaletteWheelLayout(clientRect, wheel_.center, wheelEntryCount, VisibleToolbarHeight(), dpi);
            const int radius = layout.outerRadius
                + PaletteWheelShadowInset(dpi)
                + PaletteWheelHoverOffset(dpi)
                + PaletteWheelHoverGrow(dpi)
                + ScaleForDpi(4, dpi);
            includeBounds(MakeRect(
                layout.center.x - radius,
                layout.center.y - radius,
                layout.center.x + radius,
                layout.center.y + radius));
        }
    }

    if (!overlayBounds.has_value() || !HasRect(*overlayBounds)) {
        DetachPopupComposition();
        return true;
    }
    if (!EnsureCompositionDevice()) {
        return false;
    }

    const RECT bounds = *overlayBounds;
    const int width = std::max(1, static_cast<int>(bounds.right - bounds.left));
    const int height = std::max(1, static_cast<int>(bounds.bottom - bounds.top));
    if (popupCompositionSurface_ == nullptr
        || popupCompositionWidth_ != width
        || popupCompositionHeight_ != height
        || popupCompositionDpi_ != dpi) {
        ReleaseCom(popupCompositionSurface_);
        popupCompositionContentAttached_ = false;
        const HRESULT hr = compositionDevice_->CreateSurface(
            static_cast<UINT>(width),
            static_cast<UINT>(height),
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED,
            &popupCompositionSurface_);
        if (FAILED(hr) || popupCompositionSurface_ == nullptr) {
            ReleaseCom(popupCompositionSurface_);
            return false;
        }
        popupCompositionWidth_ = width;
        popupCompositionHeight_ = height;
        popupCompositionDpi_ = dpi;
    }

    HDC blackDc = CreateCompatibleDC(nullptr);
    HDC whiteDc = CreateCompatibleDC(nullptr);
    if (blackDc == nullptr || whiteDc == nullptr) {
        if (blackDc != nullptr) {
            DeleteDC(blackDc);
        }
        if (whiteDc != nullptr) {
            DeleteDC(whiteDc);
        }
        return false;
    }

    void* blackBits = nullptr;
    void* whiteBits = nullptr;
    HBITMAP blackBitmap = CreateTopDown32BitDibSection(blackDc, width, height, &blackBits);
    HBITMAP whiteBitmap = CreateTopDown32BitDibSection(whiteDc, width, height, &whiteBits);
    if (blackBitmap == nullptr || blackBits == nullptr || whiteBitmap == nullptr || whiteBits == nullptr) {
        if (blackBitmap != nullptr) {
            DeleteObject(blackBitmap);
        }
        if (whiteBitmap != nullptr) {
            DeleteObject(whiteBitmap);
        }
        DeleteDC(blackDc);
        DeleteDC(whiteDc);
        return false;
    }

    HGDIOBJ previousBlackBitmap = SelectObject(blackDc, blackBitmap);
    HGDIOBJ previousWhiteBitmap = SelectObject(whiteDc, whiteBitmap);
    const auto releaseOverlayBitmaps = [&]() {
        if (blackDc != nullptr && previousBlackBitmap != nullptr) {
            SelectObject(blackDc, previousBlackBitmap);
        }
        if (whiteDc != nullptr && previousWhiteBitmap != nullptr) {
            SelectObject(whiteDc, previousWhiteBitmap);
        }
        if (blackBitmap != nullptr) {
            DeleteObject(blackBitmap);
        }
        if (whiteBitmap != nullptr) {
            DeleteObject(whiteBitmap);
        }
        if (blackDc != nullptr) {
            DeleteDC(blackDc);
        }
        if (whiteDc != nullptr) {
            DeleteDC(whiteDc);
        }
    };
    auto* pixels = static_cast<std::uint8_t*>(blackBits);
    auto* whitePixels = static_cast<std::uint8_t*>(whiteBits);
    const std::size_t totalPixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t index = 0; index < totalPixels; ++index) {
        pixels[(index * 4U) + 0U] = 0;
        pixels[(index * 4U) + 1U] = 0;
        pixels[(index * 4U) + 2U] = 0;
        pixels[(index * 4U) + 3U] = 0;
        whitePixels[(index * 4U) + 0U] = 255;
        whitePixels[(index * 4U) + 1U] = 255;
        whitePixels[(index * 4U) + 2U] = 255;
        whitePixels[(index * 4U) + 3U] = 0;
    }

    const auto paintOverlay = [&](HDC hdc) {
        const int savedDc = SaveDC(hdc);
        SetViewportOrgEx(hdc, -bounds.left, -bounds.top, nullptr);
        IntersectClipRect(hdc, bounds.left, bounds.top, bounds.right, bounds.bottom);
        PaintQuickSizePopup(hdc, clientRect);
        PaintPaletteMenu(hdc, clientRect);
        PaintPaletteWheel(hdc, clientRect);
        if (savedDc != 0) {
            RestoreDC(hdc, savedDc);
        }
    };
    paintOverlay(blackDc);
    paintOverlay(whiteDc);

    for (std::size_t index = 0; index < totalPixels; ++index) {
        std::uint8_t* pixel = pixels + (index * 4U);
        const std::uint8_t* whitePixel = whitePixels + (index * 4U);
        const int blueDelta = static_cast<int>(whitePixel[0]) - static_cast<int>(pixel[0]);
        const int greenDelta = static_cast<int>(whitePixel[1]) - static_cast<int>(pixel[1]);
        const int redDelta = static_cast<int>(whitePixel[2]) - static_cast<int>(pixel[2]);
        const int alpha = std::clamp(255 - ((blueDelta + greenDelta + redDelta) / 3), 0, 255);
        if (alpha == 0) {
            pixel[0] = 0;
            pixel[1] = 0;
            pixel[2] = 0;
            pixel[3] = 0;
        } else {
            pixel[0] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(pixel[0]), 0, alpha));
            pixel[1] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(pixel[1]), 0, alpha));
            pixel[2] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(pixel[2]), 0, alpha));
            pixel[3] = static_cast<std::uint8_t>(alpha);
        }
    }

    RECT updateRect { 0, 0, width, height };
    IDXGISurface* dxgiSurface = nullptr;
    POINT updateOffset {};
    HRESULT hr = popupCompositionSurface_->BeginDraw(
        &updateRect,
        __uuidof(IDXGISurface),
        reinterpret_cast<void**>(&dxgiSurface),
        &updateOffset);
    if (FAILED(hr) || dxgiSurface == nullptr) {
        releaseOverlayBitmaps();
        ReleaseCom(dxgiSurface);
        return false;
    }

    IDXGISurface1* gdiSurface = nullptr;
    hr = dxgiSurface->QueryInterface(__uuidof(IDXGISurface1), reinterpret_cast<void**>(&gdiSurface));
    ReleaseCom(dxgiSurface);
    if (FAILED(hr) || gdiSurface == nullptr) {
        popupCompositionSurface_->EndDraw();
        releaseOverlayBitmaps();
        ReleaseCom(gdiSurface);
        return false;
    }

    HDC popupDc = nullptr;
    hr = gdiSurface->GetDC(FALSE, &popupDc);
    if (FAILED(hr) || popupDc == nullptr) {
        popupCompositionSurface_->EndDraw();
        releaseOverlayBitmaps();
        ReleaseCom(gdiSurface);
        return false;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    const int copiedLines = SetDIBitsToDevice(
        popupDc,
        updateOffset.x,
        updateOffset.y,
        static_cast<DWORD>(width),
        static_cast<DWORD>(height),
        0,
        0,
        0,
        static_cast<UINT>(height),
        pixels,
        &bitmapInfo,
        DIB_RGB_COLORS);

    gdiSurface->ReleaseDC(nullptr);
    ReleaseCom(gdiSurface);
    popupCompositionSurface_->EndDraw();
    releaseOverlayBitmaps();
    if (copiedLines == 0) {
        return false;
    }

    popupCompositionVisual_->SetOffsetX(static_cast<float>(bounds.left));
    popupCompositionVisual_->SetOffsetY(static_cast<float>(bounds.top));
    popupCompositionVisual_->SetContent(popupCompositionSurface_);
    popupCompositionContentAttached_ = true;
    compositionDevice_->Commit();
    return true;
}

bool App::UpdateToolbarComposition(RECT clientRect) {
    if (!IsToolbarVisible()) {
        if (compositionDevice_ != nullptr && toolbarCompositionVisual_ != nullptr && toolbarCompositionContentAttached_) {
            toolbarCompositionVisual_->SetOffsetY(ToolbarOffsetYForProgress(std::max(1, FullToolbarHeight()), 0.0f));
            toolbarCompositionVisual_->SetContent(nullptr);
            compositionDevice_->Commit();
            compositionDevice_->WaitForCommitCompletion();
            toolbarCompositionContentAttached_ = false;
        }
        return true;
    }

    if (!EnsureCompositionDevice()) {
        return false;
    }

    const UINT dpi = WindowDpi(window_);
    const int toolbarWidth = std::max(1, static_cast<int>(clientRect.right - clientRect.left));
    const int toolbarHeight = std::max(1, FullToolbarHeight());
    const int toolbarSurfaceHeight = ToolbarVisibleClipHeight(toolbarHeight);
    toolbarViewportCompositionVisual_->SetOffsetX(0.0f);
    toolbarViewportCompositionVisual_->SetOffsetY(0.0f);

    const bool maximized = IsZoomed(window_) != FALSE;
    if (toolbarCompositionSurface_ == nullptr
        || toolbarCompositionWidth_ != toolbarWidth
        || toolbarCompositionHeight_ != toolbarSurfaceHeight
        || toolbarCompositionDpi_ != dpi
        || toolbarCompositionMaximized_ != maximized) {
        ReleaseCom(toolbarCompositionSurface_);
        toolbarCompositionContentAttached_ = false;
        toolbarCompositionDirty_ = true;
        toolbarCompositionSignature_ = 0;
        const HRESULT hr = compositionDevice_->CreateSurface(
            static_cast<UINT>(toolbarWidth),
            static_cast<UINT>(toolbarSurfaceHeight),
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_IGNORE,
            &toolbarCompositionSurface_);
        if (FAILED(hr) || toolbarCompositionSurface_ == nullptr) {
            ReleaseCom(toolbarCompositionSurface_);
            return false;
        }
        toolbarCompositionWidth_ = toolbarWidth;
        toolbarCompositionHeight_ = toolbarSurfaceHeight;
        toolbarCompositionDpi_ = dpi;
        toolbarCompositionMaximized_ = maximized;
    }

    const std::uint64_t nextSignature = ToolbarRenderSignature(clientRect);
    const bool shouldRepaintToolbar = toolbarCompositionDirty_ || toolbarCompositionSignature_ != nextSignature;
    if (shouldRepaintToolbar) {
        RECT updateRect { 0, 0, toolbarWidth, toolbarSurfaceHeight };
        IDXGISurface* dxgiSurface = nullptr;
        POINT updateOffset {};
        HRESULT hr = toolbarCompositionSurface_->BeginDraw(
            &updateRect,
            __uuidof(IDXGISurface),
            reinterpret_cast<void**>(&dxgiSurface),
            &updateOffset);
        if (FAILED(hr) || dxgiSurface == nullptr) {
            ReleaseCom(dxgiSurface);
            return false;
        }

        IDXGISurface1* gdiSurface = nullptr;
        hr = dxgiSurface->QueryInterface(__uuidof(IDXGISurface1), reinterpret_cast<void**>(&gdiSurface));
        ReleaseCom(dxgiSurface);
        if (FAILED(hr) || gdiSurface == nullptr) {
            toolbarCompositionSurface_->EndDraw();
            ReleaseCom(gdiSurface);
            return false;
        }

        HDC toolbarDc = nullptr;
        hr = gdiSurface->GetDC(FALSE, &toolbarDc);
        if (FAILED(hr) || toolbarDc == nullptr) {
            toolbarCompositionSurface_->EndDraw();
            ReleaseCom(gdiSurface);
            return false;
        }

        const int savedDc = SaveDC(toolbarDc);
        SetViewportOrgEx(toolbarDc, updateOffset.x, updateOffset.y, nullptr);
        PaintToolbarContents(toolbarDc, updateRect);
        if (savedDc != 0) {
            RestoreDC(toolbarDc, savedDc);
        }

        gdiSurface->ReleaseDC(nullptr);
        ReleaseCom(gdiSurface);
        toolbarCompositionSurface_->EndDraw();
        toolbarCompositionDirty_ = false;
        toolbarCompositionSignature_ = nextSignature;
    }

    if (!toolbarCompositionContentAttached_) {
        toolbarCompositionVisual_->SetContent(toolbarCompositionSurface_);
        toolbarCompositionContentAttached_ = true;
    }
    if (!toolbarCompositionAnimationActive_) {
        const float revealProgress = settings_.autoHideToolbar ? std::clamp(toolbarRevealProgress_, 0.0f, 1.0f) : 1.0f;
        toolbarCompositionVisual_->SetOffsetY(ToolbarOffsetYForProgress(toolbarHeight, revealProgress));
    }
    toolbarCompositionVisual_->SetOffsetX(0.0f);
    compositionDevice_->Commit();
    return true;
}

bool App::StartToolbarCompositionAnimation(float startProgress, float targetProgress) {
    if (window_ == nullptr) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    if (!UpdateToolbarComposition(clientRect) || compositionDevice_ == nullptr || toolbarCompositionVisual_ == nullptr) {
        return false;
    }

    const int toolbarHeight = std::max(1, FullToolbarHeight());
    const float startOffset = ToolbarOffsetYForProgress(toolbarHeight, startProgress);
    const float targetOffset = targetProgress <= 0.001f
        ? ToolbarHideAnimationTargetOffsetY(toolbarHeight)
        : ToolbarOffsetYForProgress(toolbarHeight, targetProgress);
    if (std::abs(startOffset - targetOffset) <= 0.001f) {
        toolbarCompositionVisual_->SetOffsetY(targetOffset);
        compositionDevice_->Commit();
        return true;
    }

    IDCompositionAnimation* animation = nullptr;
    HRESULT hr = compositionDevice_->CreateAnimation(&animation);
    if (FAILED(hr) || animation == nullptr) {
        ReleaseCom(animation);
        return false;
    }

    const double durationSeconds = static_cast<double>(kToolbarRevealDurationMs) / 1000.0;
    const float delta = targetOffset - startOffset;
    const bool hiding = targetProgress <= 0.001f;
    hr = animation->Reset();
    if (SUCCEEDED(hr) && hiding) {
        hr = animation->AddCubic(
            0.0,
            startOffset,
            0.0f,
            0.0f,
            delta * static_cast<float>(1.0 / (durationSeconds * durationSeconds * durationSeconds)));
    } else if (SUCCEEDED(hr)) {
        hr = animation->AddCubic(
            0.0,
            startOffset,
            0.0f,
            delta * static_cast<float>(3.0 / (durationSeconds * durationSeconds)),
            delta * static_cast<float>(-2.0 / (durationSeconds * durationSeconds * durationSeconds)));
    }
    if (SUCCEEDED(hr)) {
        hr = animation->End(durationSeconds, targetOffset);
    }
    if (SUCCEEDED(hr)) {
        hr = toolbarCompositionVisual_->SetOffsetY(animation);
    }
    ReleaseCom(animation);
    if (FAILED(hr)) {
        return false;
    }

    toolbarCompositionVisual_->SetOffsetX(0.0f);
    compositionDevice_->Commit();
    toolbarCompositionAnimationActive_ = true;
    return true;
}

bool App::UpdatePdfSidebarComposition(RECT clientRect) {
    if (!IsPdfSidebarVisible()) {
        if (compositionDevice_ != nullptr && pdfSidebarCompositionVisual_ != nullptr) {
            pdfSidebarCompositionVisual_->SetContent(nullptr);
            compositionDevice_->Commit();
        }
        return true;
    }

    if (!EnsureCompositionDevice()) {
        return false;
    }

    const UINT dpi = WindowDpi(window_);
    const int sidebarWidth = std::max(1, BuildSidebarMetrics(dpi).width);
    const int sidebarTop = ReservedToolbarHeight();
    const int sidebarHeight = std::max(1, static_cast<int>(clientRect.bottom) - sidebarTop);
    if (pdfSidebarCompositionSurface_ == nullptr
        || pdfSidebarCompositionWidth_ != sidebarWidth
        || pdfSidebarCompositionHeight_ != sidebarHeight
        || pdfSidebarCompositionDpi_ != dpi) {
        ReleaseCom(pdfSidebarCompositionSurface_);
        const HRESULT hr = compositionDevice_->CreateSurface(
            static_cast<UINT>(sidebarWidth),
            static_cast<UINT>(sidebarHeight),
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_IGNORE,
            &pdfSidebarCompositionSurface_);
        if (FAILED(hr) || pdfSidebarCompositionSurface_ == nullptr) {
            ReleaseCom(pdfSidebarCompositionSurface_);
            return false;
        }
        pdfSidebarCompositionWidth_ = sidebarWidth;
        pdfSidebarCompositionHeight_ = sidebarHeight;
        pdfSidebarCompositionDpi_ = dpi;
        pdfSidebarCompositionDirty_ = true;
    }

    if (pdfSidebarCompositionDirty_) {
        RECT updateRect { 0, 0, sidebarWidth, sidebarHeight };
        IDXGISurface* dxgiSurface = nullptr;
        POINT updateOffset {};
        HRESULT hr = pdfSidebarCompositionSurface_->BeginDraw(
            &updateRect,
            __uuidof(IDXGISurface),
            reinterpret_cast<void**>(&dxgiSurface),
            &updateOffset);
        if (FAILED(hr) || dxgiSurface == nullptr) {
            ReleaseCom(dxgiSurface);
            return false;
        }

        IDXGISurface1* gdiSurface = nullptr;
        hr = dxgiSurface->QueryInterface(__uuidof(IDXGISurface1), reinterpret_cast<void**>(&gdiSurface));
        ReleaseCom(dxgiSurface);
        if (FAILED(hr) || gdiSurface == nullptr) {
            pdfSidebarCompositionSurface_->EndDraw();
            ReleaseCom(gdiSurface);
            return false;
        }

        HDC sidebarDc = nullptr;
        hr = gdiSurface->GetDC(FALSE, &sidebarDc);
        if (FAILED(hr) || sidebarDc == nullptr) {
            pdfSidebarCompositionSurface_->EndDraw();
            ReleaseCom(gdiSurface);
            return false;
        }

        const int savedDc = SaveDC(sidebarDc);
        SetViewportOrgEx(sidebarDc, updateOffset.x, updateOffset.y, nullptr);
        PaintPdfSidebarContents(sidebarDc, updateRect);
        if (savedDc != 0) {
            RestoreDC(sidebarDc, savedDc);
        }

        gdiSurface->ReleaseDC(nullptr);
        ReleaseCom(gdiSurface);
        pdfSidebarCompositionSurface_->EndDraw();
        pdfSidebarCompositionDirty_ = false;
    }

    pdfSidebarCompositionVisual_->SetContent(pdfSidebarCompositionSurface_);
    if (!pdfSidebarCompositionAnimationActive_) {
        pdfSidebarCompositionVisual_->SetOffsetX(PdfSidebarOffsetXForProgress(sidebarWidth, dpi, pdfSidebarSlideProgress_));
    }
    pdfSidebarCompositionVisual_->SetOffsetY(static_cast<float>(sidebarTop));
    compositionDevice_->Commit();
    return true;
}

bool App::StartPdfSidebarCompositionAnimation(float startProgress, float targetProgress) {
    if (window_ == nullptr) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    if (!UpdatePdfSidebarComposition(clientRect) || compositionDevice_ == nullptr || pdfSidebarCompositionVisual_ == nullptr) {
        return false;
    }

    const UINT dpi = WindowDpi(window_);
    const int sidebarWidth = std::max(1, BuildSidebarMetrics(dpi).width);
    const float startOffset = PdfSidebarOffsetXForProgress(sidebarWidth, dpi, startProgress);
    const float targetOffset = PdfSidebarOffsetXForProgress(sidebarWidth, dpi, targetProgress);
    if (std::abs(startOffset - targetOffset) <= 0.001f) {
        pdfSidebarCompositionVisual_->SetOffsetX(targetOffset);
        compositionDevice_->Commit();
        return true;
    }

    IDCompositionAnimation* animation = nullptr;
    HRESULT hr = compositionDevice_->CreateAnimation(&animation);
    if (FAILED(hr) || animation == nullptr) {
        ReleaseCom(animation);
        return false;
    }

    const double durationSeconds = static_cast<double>(kPdfSidebarSlideDurationMs) / 1000.0;
    const float delta = targetOffset - startOffset;
    hr = animation->Reset();
    if (SUCCEEDED(hr)) {
        hr = animation->AddCubic(
            0.0,
            startOffset,
            0.0f,
            delta * static_cast<float>(3.0 / (durationSeconds * durationSeconds)),
            delta * static_cast<float>(-2.0 / (durationSeconds * durationSeconds * durationSeconds)));
    }
    if (SUCCEEDED(hr)) {
        hr = animation->End(durationSeconds, targetOffset);
    }
    if (SUCCEEDED(hr)) {
        hr = pdfSidebarCompositionVisual_->SetOffsetX(animation);
    }
    ReleaseCom(animation);
    if (FAILED(hr)) {
        return false;
    }

    pdfSidebarCompositionVisual_->SetOffsetY(static_cast<float>(ReservedToolbarHeight()));
    compositionDevice_->Commit();
    pdfSidebarCompositionAnimationActive_ = true;
    return true;
}

bool App::SidebarOwnsKeyboardShortcuts() const {
    if (!pdfSidebarOpen_ || !IsPdfSidebarVisible() || window_ == nullptr) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT sidebarRect = PdfSidebarRect(clientRect);
    POINT cursor {};
    GetCursorPos(&cursor);
    ScreenToClient(window_, &cursor);
    return PtInRect(&sidebarRect, cursor) != 0;
}

int App::PdfSidebarWidth() const {
    return IsPdfSidebarVisible() ? BuildSidebarMetrics(WindowDpi(window_)).width : 0;
}

RECT App::PdfSidebarRect(RECT clientRect) const {
    RECT rect = clientRect;
    const UINT dpi = WindowDpi(window_);
    const int horizontalInset = IsZoomed(window_) ? WindowFrameThicknessX(dpi) : 0;
    const int sidebarWidth = PdfSidebarWidth();
    const float slideProgress = std::clamp(pdfSidebarSlideProgress_, 0.0f, 1.0f);
    rect.top = ReservedToolbarHeight();
    rect.left += horizontalInset - static_cast<int>(std::lround(static_cast<float>(sidebarWidth) * (1.0f - slideProgress)));
    rect.right = rect.left + sidebarWidth;
    return rect;
}

void App::InvalidatePdfSidebarAnimationRegion() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    if (UpdatePdfSidebarComposition(clientRect)) {
        return;
    }

    const UINT dpi = WindowDpi(window_);
    const int horizontalInset = IsZoomed(window_) ? WindowFrameThicknessX(dpi) : 0;
    const int sidebarWidth = BuildSidebarMetrics(dpi).width;
    RECT dirtyRect = clientRect;
    dirtyRect.left = 0;
    dirtyRect.top = ReservedToolbarHeight();
    dirtyRect.right = std::min<LONG>(clientRect.right, horizontalInset + sidebarWidth);
    if (dirtyRect.right > dirtyRect.left && dirtyRect.bottom > dirtyRect.top) {
        RedrawWindow(window_, &dirtyRect, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    }
}

void App::StartPdfSidebarSlideAnimation(float targetProgress) {
    targetProgress = std::clamp(targetProgress, 0.0f, 1.0f);
    if (window_ == nullptr) {
        pdfSidebarSlideProgress_ = targetProgress;
        StopPdfSidebarSlideAnimationTimer();
        return;
    }

    if (pdfSidebarSlideAnimationStartMs_ > 0) {
        const ULONGLONG nowMs = GetTickCount64();
        const double elapsed = static_cast<double>(nowMs - pdfSidebarSlideAnimationStartMs_);
        const float linearT = static_cast<float>(std::clamp(elapsed / static_cast<double>(kPdfSidebarSlideDurationMs), 0.0, 1.0));
        const float easedT = linearT * linearT * (3.0f - (2.0f * linearT));
        pdfSidebarSlideProgress_ = pdfSidebarSlideAnimationStartProgress_
            + ((pdfSidebarSlideAnimationTargetProgress_ - pdfSidebarSlideAnimationStartProgress_) * easedT);
    }
    StopPdfSidebarSlideAnimationTimer();

    if (std::abs(pdfSidebarSlideProgress_ - targetProgress) <= 0.001f) {
        pdfSidebarSlideProgress_ = targetProgress;
        pdfSidebarSlideAnimationStartProgress_ = targetProgress;
        pdfSidebarSlideAnimationTargetProgress_ = targetProgress;
        pdfSidebarSlideAnimationStartMs_ = 0;
        InvalidatePdfSidebarAnimationRegion();
    } else {
        pdfSidebarSlideAnimationStartProgress_ = pdfSidebarSlideProgress_;
        pdfSidebarSlideAnimationTargetProgress_ = targetProgress;
        pdfSidebarSlideAnimationStartMs_ = GetTickCount64();
        if (StartPdfSidebarCompositionAnimation(pdfSidebarSlideAnimationStartProgress_, targetProgress)) {
            SetTimer(window_, kPdfSidebarSlideTimerId, kPdfSidebarSlideDurationMs + 32, nullptr);
            return;
        }

        if (timeBeginPeriod(1) == TIMERR_NOERROR) {
            pdfSidebarSlideTimerResolutionActive_ = true;
        }
        SetTimer(window_, kPdfSidebarSlideTimerId, kPdfSidebarSlideFrameMs, nullptr);
        InvalidatePdfSidebarAnimationRegion();
    }
}

void App::StopPdfSidebarSlideAnimationTimer() {
    if (window_ != nullptr) {
        KillTimer(window_, kPdfSidebarSlideTimerId);
    }
    pdfSidebarCompositionAnimationActive_ = false;
    if (pdfSidebarSlideTimerResolutionActive_) {
        timeEndPeriod(1);
        pdfSidebarSlideTimerResolutionActive_ = false;
    }
}

void App::HandlePdfSidebarSlideAnimationTimer() {
    if (window_ == nullptr) {
        return;
    }

    if (pdfSidebarCompositionAnimationActive_) {
        pdfSidebarSlideProgress_ = pdfSidebarSlideAnimationTargetProgress_;
        pdfSidebarSlideAnimationStartProgress_ = pdfSidebarSlideProgress_;
        pdfSidebarSlideAnimationStartMs_ = 0;
        StopPdfSidebarSlideAnimationTimer();
        InvalidatePdfSidebarAnimationRegion();
        return;
    }

    const ULONGLONG startMs = pdfSidebarSlideAnimationStartMs_;
    const ULONGLONG nowMs = GetTickCount64();
    const double elapsed = startMs > 0 ? static_cast<double>(nowMs - startMs) : static_cast<double>(kPdfSidebarSlideDurationMs);
    const float linearT = static_cast<float>(std::clamp(elapsed / static_cast<double>(kPdfSidebarSlideDurationMs), 0.0, 1.0));
    const float easedT = linearT * linearT * (3.0f - (2.0f * linearT));
    pdfSidebarSlideProgress_ = pdfSidebarSlideAnimationStartProgress_
        + ((pdfSidebarSlideAnimationTargetProgress_ - pdfSidebarSlideAnimationStartProgress_) * easedT);

    InvalidatePdfSidebarAnimationRegion();
    DwmFlush();

    if (linearT >= 1.0f) {
        pdfSidebarSlideProgress_ = pdfSidebarSlideAnimationTargetProgress_;
        pdfSidebarSlideAnimationStartProgress_ = pdfSidebarSlideProgress_;
        StopPdfSidebarSlideAnimationTimer();
        InvalidatePdfSidebarAnimationRegion();
    }
}


std::optional<std::size_t> App::HitTestSidebarPage(POINT cursor) const {
    if (!pdfSidebarOpen_ || !IsPdfSidebarVisible()) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT sidebarRect = PdfSidebarRect(clientRect);
    if (PtInRect(&sidebarRect, cursor) == 0) {
        return std::nullopt;
    }

    const UINT dpi = WindowDpi(window_);
    const SidebarHeaderLayout headerLayout = BuildSidebarHeaderLayout(sidebarRect, dpi, IsZoomed(window_) != FALSE);
    const SidebarMetrics metrics = BuildSidebarMetrics(dpi);
    const int itemWidth = std::max(1, static_cast<int>(headerLayout.headerRect.right - headerLayout.headerRect.left) - (metrics.itemInset * 2));
    const int left = ((sidebarRect.left + sidebarRect.right) - itemWidth) / 2;
    int top = headerLayout.contentRect.top + metrics.contentPadding - pdfSidebarScrollY_;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        const Page& page = document_.PageAt(pageIndex);
        const float aspect = page.width > 0.0f ? (page.height / page.width) : (11.0f / 8.5f);
        const int thumbHeight = std::max(metrics.minThumbHeight, static_cast<int>(std::lround(static_cast<float>(itemWidth) * aspect)));
        RECT itemRect = MakeRect(left, top, left + itemWidth, top + thumbHeight + metrics.labelHeight);
        if (PtInRect(&itemRect, cursor) != 0) {
            return pageIndex;
        }
        top = itemRect.bottom + metrics.itemGap;
    }

    return std::nullopt;
}

void App::EnsureSidebarCurrentPageVisible() {
    if (!IsPdfSidebarVisible() || window_ == nullptr || document_.PageCount() == 0U) {
        return;
    }

    const int previousScrollY = pdfSidebarScrollY_;
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT sidebarRect = PdfSidebarRect(clientRect);
    const UINT dpi = WindowDpi(window_);
    const SidebarHeaderLayout headerLayout = BuildSidebarHeaderLayout(sidebarRect, dpi, IsZoomed(window_) != FALSE);
    const SidebarMetrics metrics = BuildSidebarMetrics(dpi);
    const int itemWidth = std::max(1, static_cast<int>(headerLayout.headerRect.right - headerLayout.headerRect.left) - (metrics.itemInset * 2));
    const int left = ((sidebarRect.left + sidebarRect.right) - itemWidth) / 2;
    int top = headerLayout.contentRect.top + metrics.contentPadding;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        const Page& page = document_.PageAt(pageIndex);
        const float aspect = page.width > 0.0f ? (page.height / page.width) : (11.0f / 8.5f);
        const int thumbHeight = std::max(metrics.minThumbHeight, static_cast<int>(std::lround(static_cast<float>(itemWidth) * aspect)));
        RECT itemRect = MakeRect(left, top, left + itemWidth, top + thumbHeight + metrics.labelHeight);
        if (pageIndex == document_.CurrentPageIndex()) {
            const int visibleTop = headerLayout.contentRect.top + metrics.contentPadding + pdfSidebarScrollY_;
            const int visibleBottom = headerLayout.contentRect.bottom - metrics.contentPadding + pdfSidebarScrollY_;
            if (itemRect.top < visibleTop) {
                pdfSidebarScrollY_ = std::max(0, static_cast<int>(itemRect.top - headerLayout.contentRect.top - metrics.contentPadding));
            } else if (itemRect.bottom > visibleBottom) {
                pdfSidebarScrollY_ = std::max(0, static_cast<int>(itemRect.bottom - (headerLayout.contentRect.bottom - metrics.contentPadding)));
            }
            if (pdfSidebarScrollY_ != previousScrollY) {
                MarkPdfSidebarCompositionDirty();
            }
            return;
        }
        top = itemRect.bottom + metrics.itemGap;
    }
}

void App::SyncCurrentPageToViewport() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const auto viewedPageIndex = ViewedPageIndex(clientRect);
    if (!viewedPageIndex.has_value() || *viewedPageIndex == document_.CurrentPageIndex()) {
        return;
    }

    document_.SetCurrentPageIndex(*viewedPageIndex);
    MarkPdfSidebarCompositionDirty();
    EnsureSidebarCurrentPageVisible();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::InvalidateSidebarThumbnailCache(std::size_t pageIndex) {
    if (pageIndex >= sidebarThumbnailCaches_.size()) {
        return;
    }

    SidebarThumbnailCache& cache = sidebarThumbnailCaches_[pageIndex];
    cache.valid = false;
    cache.pixels.clear();
    cache.width = 0;
    cache.height = 0;
    cache.stride = 0;
    cache.sourceWidth = 0;
    cache.sourceHeight = 0;
    cache.sourceScale = 0.0f;
    cache.sourcePdfSourceIndex = -1;
    cache.sourcePdfPageIndex = -1;
    MarkPdfSidebarCompositionDirty();
}

void App::EnsureSidebarThumbnailCache(std::size_t pageIndex, int width, int height) {
    if (pageIndex >= document_.PageCount() || width <= 0 || height <= 0) {
        return;
    }

    if (pageIndex >= sidebarThumbnailCaches_.size()) {
        sidebarThumbnailCaches_.resize(document_.PageCount());
    }

    const Page& page = document_.PageAt(pageIndex);
    SidebarThumbnailCache& cache = sidebarThumbnailCaches_[pageIndex];
    if (cache.valid
        && cache.width == width
        && cache.height == height
        && cache.sourceWidth == page.raster.width
        && cache.sourceHeight == page.raster.height
        && std::abs(cache.sourceScale - page.raster.scale) < 0.001f
        && cache.sourcePdfSourceIndex == page.sourcePdfSourceIndex
        && cache.sourcePdfPageIndex == page.sourcePdfPageIndex) {
        return;
    }

    cache.width = width;
    cache.height = height;
    cache.stride = width * 4;
    cache.sourceWidth = page.raster.width;
    cache.sourceHeight = page.raster.height;
    cache.sourceScale = page.raster.scale;
    cache.sourcePdfSourceIndex = page.sourcePdfSourceIndex;
    cache.sourcePdfPageIndex = page.sourcePdfPageIndex;
    cache.pixels.assign(static_cast<std::size_t>(cache.stride * cache.height), 255);

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HDC cacheDc = CreateCompatibleDC(nullptr);
    if (cacheDc == nullptr) {
        InvalidateSidebarThumbnailCache(pageIndex);
        return;
    }

    void* bits = nullptr;
    HBITMAP cacheBitmap = CreateDIBSection(cacheDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (cacheBitmap == nullptr || bits == nullptr) {
        if (cacheBitmap != nullptr) {
            DeleteObject(cacheBitmap);
        }
        DeleteDC(cacheDc);
        InvalidateSidebarThumbnailCache(pageIndex);
        return;
    }

    HGDIOBJ previousBitmap = SelectObject(cacheDc, cacheBitmap);
    RECT cacheRect = MakeRect(0, 0, width, height);
    HBRUSH pageBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(cacheDc, &cacheRect, pageBrush);
    DeleteObject(pageBrush);

    const bool drewNativeBackground = PaintNativePdfBackground(cacheDc, page, cacheRect);

    Gdiplus::Graphics imageGraphics(cacheDc);
    imageGraphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    imageGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    imageGraphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    if (!drewNativeBackground
        && !page.raster.pixels.empty()
        && page.raster.width > 0
        && page.raster.height > 0) {
        Gdiplus::Bitmap bitmap(
            page.raster.width,
            page.raster.height,
            page.raster.stride,
            PixelFormat32bppRGB,
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(page.raster.pixels.data())));
        imageGraphics.DrawImage(
            &bitmap,
            Gdiplus::Rect(0, 0, width, height),
            0,
            0,
            page.raster.width,
            page.raster.height,
            Gdiplus::UnitPixel);
    }

    const float scaleX = width > 0 ? static_cast<float>(width) / std::max(1.0f, page.width) : 1.0f;
    const float scaleY = height > 0 ? static_cast<float>(height) / std::max(1.0f, page.height) : 1.0f;
    const PageView cacheView {
        MakeRect(0, 0, width, height),
        pageIndex,
        std::min(scaleX, scaleY),
        true,
    };

    for (const PageImage& image : page.images) {
        if (image.pixels.empty() || image.pixelWidth <= 0 || image.pixelHeight <= 0) {
            continue;
        }

        Gdiplus::Bitmap bitmap(
            image.pixelWidth,
            image.pixelHeight,
            image.stride,
            PixelFormat32bppRGB,
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(image.pixels.data())));
        const Gdiplus::REAL centerX = static_cast<Gdiplus::REAL>((image.x + (image.width * 0.5f)) * cacheView.scale);
        const Gdiplus::REAL centerY = static_cast<Gdiplus::REAL>((image.y + (image.height * 0.5f)) * cacheView.scale);
        const Gdiplus::REAL drawWidth = static_cast<Gdiplus::REAL>(image.width * cacheView.scale);
        const Gdiplus::REAL drawHeight = static_cast<Gdiplus::REAL>(image.height * cacheView.scale);

        imageGraphics.ResetTransform();
        imageGraphics.TranslateTransform(centerX, centerY);
        imageGraphics.RotateTransform(image.rotationDegrees);
        imageGraphics.DrawImage(&bitmap, -drawWidth * 0.5f, -drawHeight * 0.5f, drawWidth, drawHeight);
    }
    imageGraphics.ResetTransform();
    for (const Stroke& stroke : page.strokes) {
        PaintStroke(cacheDc, stroke, cacheView, false);
    }

    std::copy_n(static_cast<const std::uint8_t*>(bits), cache.pixels.size(), cache.pixels.data());
    SelectObject(cacheDc, previousBitmap);
    DeleteObject(cacheBitmap);
    DeleteDC(cacheDc);

    cache.valid = true;
}

void App::UpdateWindowTitle() {
    if (window_ != nullptr) {
        const std::wstring title = CurrentWindowTitle();
        SetWindowTextW(window_, title.c_str());
    }
}

void App::BeginStroke(POINT cursor) {
    if (isPanning_ || isErasing_) {
        return;
    }

    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        return;
    }

    if (HasSelection()) {
        ClearSelection();
        InvalidateCanvas();
    }

    document_.SetCurrentPageIndex(hitPage->pageIndex);
    MarkPdfSidebarCompositionDirty();
    StartStrokeSegment(hitPage->pageIndex, ScreenToPage(*hitPage, cursor, true));
}

void App::ExtendStroke(POINT cursor) {
    const std::optional<RECT> previousDirtyRect = ActiveStrokeIncrementalWindowDirtyRect();
    if (!ApplyStrokePoint(cursor)) {
        return;
    }
    InvalidateStrokeDirtyDelta(previousDirtyRect);
}

bool App::ApplyStrokePoint(POINT cursor) {
    if (window_ == nullptr || !isDrawing_ || isPanning_ || !activeStroke_.has_value()) {
        return false;
    }

    if (!activeStrokePageIndex_.has_value()) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *activeStrokePageIndex_);
    if (!pageView.valid || pageView.scale <= 0.0f) {
        return false;
    }

    if (const auto hitPage = HitTestPage(cursor); hitPage.has_value() && hitPage->pageIndex != *activeStrokePageIndex_) {
        EndStroke();
        document_.SetCurrentPageIndex(hitPage->pageIndex);
        MarkPdfSidebarCompositionDirty();
        StartStrokeSegment(hitPage->pageIndex, ScreenToPage(*hitPage, cursor, true));
        return true;
    }

    const Vec2 rawPoint = ScreenToPage(pageView, cursor, true);
    Vec2 filteredPoint = rawPoint;
    const float inputSpacing = InputSpacingPageUnits(settings_.inputSpacing, pageView.scale);
    const bool straightLineMode = StraightLineStrokeModifierPressed();

    if (!activeFreehandStroke_.has_value()) {
        activeFreehandStroke_ = *activeStroke_;
    }

    const bool dottedMode = DottedStrokeModifierPressed();
    const bool styleChanged = activeStroke_->style.dotted != dottedMode;
    activeStroke_->style.dotted = dottedMode;
    activeFreehandStroke_->style.dotted = dottedMode;

    bool pointAppended = true;
    if (!activeFreehandStroke_->points.empty()) {
        const float followAmount = std::clamp(1.0f - settings_.inputSmoothing, 0.05f, 0.95f);
        filteredPoint = Lerp(activeFreehandStroke_->points.back(), rawPoint, followAmount);
        if (Distance(filteredPoint, activeFreehandStroke_->points.back()) < inputSpacing) {
            pointAppended = false;
        }
    }

    if (pointAppended) {
        AppendStrokePointWithResampling(*activeFreehandStroke_, filteredPoint, inputSpacing);
    }

    if (straightLineMode && !activeFreehandStroke_->points.empty()) {
        const Vec2 startPoint = activeFreehandStroke_->points.front();
        if (OvalStrokeModifierPressed()) {
            activeStroke_->points = BuildOvalStrokePoints(startPoint, rawPoint, SnapLineModifierPressed());
        } else {
            const Vec2 endPoint = SnapLineModifierPressed()
                ? SnapLineEndpoint(startPoint, rawPoint)
                : rawPoint;
            activeStroke_->points.clear();
            activeStroke_->points.push_back(startPoint);
            activeStroke_->points.push_back(endPoint);
        }
    } else {
        if (!pointAppended && !styleChanged) {
            return false;
        }
        *activeStroke_ = *activeFreehandStroke_;
    }
    return true;
}

std::optional<RECT> App::ActiveStrokeWindowDirtyRect() const {
    if (window_ == nullptr || !activeStroke_.has_value() || !activeStrokePageIndex_.has_value()) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *activeStrokePageIndex_);
    if (!pageView.valid || pageView.scale <= 0.0f) {
        return std::nullopt;
    }

    std::optional<RECT> currentDirtyRect = StrokeRenderBoundsRect(
        *activeStroke_,
        pageView.scale,
        true,
        settings_.strokeSmoothing,
        12);
    if (!currentDirtyRect.has_value()) {
        return std::nullopt;
    }

    OffsetRect(&*currentDirtyRect, pageView.rect.left, pageView.rect.top);
    return currentDirtyRect;
}

std::optional<RECT> App::ActiveStrokeIncrementalWindowDirtyRect() const {
    if (window_ == nullptr || !activeStroke_.has_value() || !activeStrokePageIndex_.has_value()) {
        return std::nullopt;
    }

    if (StraightLineStrokeModifierPressed() || activeStroke_->style.dotted != DottedStrokeModifierPressed()) {
        return ActiveStrokeWindowDirtyRect();
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *activeStrokePageIndex_);
    if (!pageView.valid || pageView.scale <= 0.0f) {
        return std::nullopt;
    }

    constexpr std::size_t kActiveStrokeDirtyTailPointCount = 12;
    const std::vector<Vec2>& points = activeStroke_->points;
    if (points.empty()) {
        return std::nullopt;
    }

    Stroke dirtyStroke {};
    dirtyStroke.style = activeStroke_->style;
    dirtyStroke.smoothing = activeStroke_->smoothing;
    const std::size_t firstPointIndex = points.size() > kActiveStrokeDirtyTailPointCount
        ? points.size() - kActiveStrokeDirtyTailPointCount
        : 0U;
    dirtyStroke.points.assign(points.begin() + static_cast<std::ptrdiff_t>(firstPointIndex), points.end());

    std::optional<RECT> currentDirtyRect = StrokeRenderBoundsRect(
        dirtyStroke,
        pageView.scale,
        true,
        settings_.strokeSmoothing,
        18);
    if (!currentDirtyRect.has_value()) {
        return std::nullopt;
    }

    OffsetRect(&*currentDirtyRect, pageView.rect.left, pageView.rect.top);
    return currentDirtyRect;
}

void App::InvalidateStrokeDirtyDelta(const std::optional<RECT>& previousDirtyRect) {
    if (window_ == nullptr) {
        return;
    }

    std::optional<RECT> currentDirtyRect = ActiveStrokeIncrementalWindowDirtyRect();
    if (previousDirtyRect.has_value()) {
        currentDirtyRect = currentDirtyRect.has_value()
            ? UnionRectangles(*previousDirtyRect, *currentDirtyRect)
            : previousDirtyRect;
    }
    if (currentDirtyRect.has_value()) {
        InvalidateRect(window_, &*currentDirtyRect, FALSE);
    }
}

bool App::ExtendStrokeFromPointerHistory(WPARAM wParam) {
    if (!isDrawing_ || activeTool_ != Tool::Draw || window_ == nullptr || !activeStroke_.has_value()) {
        return false;
    }

    const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
    if (activeStrokePointerId_.has_value() && *activeStrokePointerId_ != pointerId) {
        return false;
    }

    UINT32 entryCount = 0;
    static_cast<void>(GetPointerInfoHistory(pointerId, &entryCount, nullptr));
    if (entryCount == 0U) {
        POINTER_INFO pointerInfo {};
        if (GetPointerInfo(pointerId, &pointerInfo) == FALSE) {
            return false;
        }

        POINT cursor = pointerInfo.ptPixelLocation;
        ScreenToClient(window_, &cursor);
        activeStrokePointerId_ = pointerId;
        ExtendStroke(cursor);
        return true;
    }

    std::vector<POINTER_INFO> history(entryCount);
    if (GetPointerInfoHistory(pointerId, &entryCount, history.data()) == FALSE || entryCount == 0U) {
        return false;
    }
    history.resize(entryCount);
    std::sort(history.begin(), history.end(), [](const POINTER_INFO& a, const POINTER_INFO& b) {
        if (a.PerformanceCount != b.PerformanceCount) {
            return a.PerformanceCount < b.PerformanceCount;
        }
        return a.frameId < b.frameId;
    });

    activeStrokePointerId_ = pointerId;
    const std::optional<RECT> previousDirtyRect = ActiveStrokeIncrementalWindowDirtyRect();
    bool strokeChanged = false;
    for (const POINTER_INFO& pointerInfo : history) {
        POINT cursor = pointerInfo.ptPixelLocation;
        ScreenToClient(window_, &cursor);
        strokeChanged = ApplyStrokePoint(cursor) || strokeChanged;
    }
    if (strokeChanged) {
        InvalidateStrokeDirtyDelta(previousDirtyRect);
    }
    return true;
}

void App::StartStrokeSegment(std::size_t pageIndex, Vec2 pagePoint) {
    if (pageIndex >= document_.PageCount()) {
        return;
    }

    activeStroke_ = Stroke {};
    activeStrokePageIndex_ = pageIndex;
    activeStrokePointerId_.reset();
    activeStroke_->style.color = palette_.at(colorIndex_);
    activeStroke_->style.width = settings_.drawBrushSize;
    activeStroke_->style.dotted = DottedStrokeModifierPressed();
    activeStroke_->smoothing = settings_.strokeSmoothing;
    activeStroke_->points.push_back(pagePoint);
    activeFreehandStroke_ = activeStroke_;
    isDrawing_ = true;
    UpdateCaptureState();
    InvalidateStrokeRegion(*activeStroke_, pageIndex, true);
}

void App::EndStroke() {
    std::optional<Stroke> committedStroke;
    std::optional<std::size_t> committedPageIndex;
    std::optional<RECT> previewDirtyRect;
    std::optional<PageView> committedPageView;
    if (isDrawing_ && activeStroke_.has_value() && activeStrokePageIndex_.has_value() && !activeStroke_->points.empty()) {
        if (window_ != nullptr) {
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            const PageView pageView = CalculatePageView(clientRect, *activeStrokePageIndex_);
            if (pageView.valid && pageView.scale > 0.0f) {
                previewDirtyRect = StrokeRenderBoundsRect(
                    *activeStroke_,
                    pageView.scale,
                    true,
                    settings_.strokeSmoothing,
                    12);
                committedPageView = pageView;
            }
        }

        Stroke finishedStroke = std::move(*activeStroke_);
        auto& strokes = document_.PageAt(*activeStrokePageIndex_).strokes;
        const std::size_t insertIndex = strokes.size();
        strokes.push_back(finishedStroke);
        HistoryAction action {};
        action.type = HistoryActionType::AddStroke;
        action.stroke = std::move(finishedStroke);
        action.index = insertIndex;
        action.pageIndex = *activeStrokePageIndex_;
        undoActions_.push_back(std::move(action));
        redoActions_.clear();
        committedStroke = undoActions_.back().stroke;
        committedPageIndex = *activeStrokePageIndex_;
        if (!TryAppendStrokeToPageSurfaceCache(*committedPageIndex, *committedStroke)) {
            InvalidatePageSurfaceCache(*committedPageIndex);
        }
    }

    activeStroke_.reset();
    activeFreehandStroke_.reset();
    activeStrokePageIndex_.reset();
    activeStrokePointerId_.reset();
    isDrawing_ = false;
    UpdateCaptureState();
    if (committedStroke.has_value() && committedPageIndex.has_value()) {
        NotifyDocumentMutated();
        if (window_ != nullptr && committedPageView.has_value()) {
            std::optional<RECT> dirtyRect = StrokeRenderBoundsRect(
                *committedStroke,
                committedPageView->scale,
                false,
                settings_.strokeSmoothing,
                12);
            if (previewDirtyRect.has_value()) {
                dirtyRect = dirtyRect.has_value()
                    ? UnionRectangles(*previewDirtyRect, *dirtyRect)
                    : previewDirtyRect;
            }
            if (dirtyRect.has_value()) {
                RECT windowDirtyRect = *dirtyRect;
                OffsetRect(&windowDirtyRect, committedPageView->rect.left, committedPageView->rect.top);
                InvalidateRect(window_, &windowDirtyRect, FALSE);
            } else {
                InvalidateStrokeRegion(*committedStroke, *committedPageIndex, false);
            }
        } else {
            InvalidateStrokeRegion(*committedStroke, *committedPageIndex, false);
        }
    } else {
        InvalidateCanvas();
    }
}

void App::BeginSelection(POINT cursor, bool boxOverride) {
    if (isPanning_) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT canvasRect = CanvasRect(clientRect);
    if (PtInRect(&canvasRect, cursor) == 0) {
        ClearSelection();
        selectionBoxOverride_ = false;
        InvalidateCanvas();
        return;
    }

    selectionAnchorCursor_ = cursor;
    selectionCurrentCursor_ = cursor;
    selectionLassoCursorPoints_.clear();
    selectionLassoCursorPoints_.push_back(cursor);
    selectionBoxOverride_ = boxOverride;
    isSelecting_ = true;
    if (const auto hitPage = HitTestPage(cursor); hitPage.has_value()) {
        document_.SetCurrentPageIndex(hitPage->pageIndex);
        MarkPdfSidebarCompositionDirty();
    }
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::ExtendSelection(POINT cursor) {
    if (!isSelecting_) {
        return;
    }

    selectionCurrentCursor_ = cursor;

    if (selectionMode_ == SelectionMode::Lasso) {
        const Vec2 currentPoint = PointToVec2(selectionCurrentCursor_);
        if (selectionLassoCursorPoints_.empty()
            || DistanceSquared(currentPoint, PointToVec2(selectionLassoCursorPoints_.back())) >= 36.0f) {
            selectionLassoCursorPoints_.push_back(selectionCurrentCursor_);
        }
    }

    InvalidateCanvas();
}

void App::EndSelection() {
    if (!isSelecting_) {
        return;
    }

    const SelectionMode effectiveSelectionMode = selectionBoxOverride_ ? SelectionMode::Box : selectionMode_;
    const bool clickSelection = DistanceSquared(
        PointToVec2(selectionAnchorCursor_),
        PointToVec2(selectionCurrentCursor_)) <= (18.0f * 18.0f);
    const std::vector<Vec2> selectionPolygon = effectiveSelectionMode == SelectionMode::Lasso
        ? CurrentSelectionLassoPolygon()
        : std::vector<Vec2> {};

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    ClearSelection();
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid || pageView.pageIndex >= document_.PageCount()) {
            continue;
        }

        const Page& page = document_.PageAt(pageView.pageIndex);
        const Vec2 anchorPage = ScreenToPage(pageView, selectionAnchorCursor_, false);
        const Vec2 currentPage = ScreenToPage(pageView, selectionCurrentCursor_, false);
        std::vector<Vec2> pagePolygon;
        if (effectiveSelectionMode == SelectionMode::Lasso) {
            pagePolygon.reserve(selectionPolygon.size());
            for (const Vec2& point : selectionPolygon) {
                const POINT screenPoint {
                    static_cast<LONG>(std::lround(point.x)),
                    static_cast<LONG>(std::lround(point.y)),
                };
                pagePolygon.push_back(ScreenToPage(pageView, screenPoint, false));
            }
        }

        for (std::size_t strokeIndex = 0; strokeIndex < page.strokes.size(); ++strokeIndex) {
            const Stroke& stroke = page.strokes[strokeIndex];
            bool hit = false;
            if (effectiveSelectionMode == SelectionMode::Box) {
                hit = clickSelection
                    ? StrokeHitsPoint(stroke, currentPage, 10.0f / std::max(0.1f, pageView.scale), settings_.strokeSmoothing)
                    : StrokeIntersectsRectSelection(stroke, anchorPage, currentPage, settings_.strokeSmoothing);
            } else {
                hit = (pagePolygon.size() < 3U)
                    ? StrokeHitsPoint(stroke, currentPage, 10.0f / std::max(0.1f, pageView.scale), settings_.strokeSmoothing)
                    : StrokeIntersectsPolygonSelection(stroke, pagePolygon, settings_.strokeSmoothing);
            }

            if (hit && pageView.pageIndex < selectedStrokeIndices_.size()) {
                selectedStrokeIndices_[pageView.pageIndex].push_back(strokeIndex);
            }
        }

        for (std::size_t imageIndex = 0; imageIndex < page.images.size(); ++imageIndex) {
            const PageImage& image = page.images[imageIndex];
            bool hit = false;
            if (effectiveSelectionMode == SelectionMode::Box) {
                hit = clickSelection
                    ? ImageHitsPoint(image, currentPage)
                    : ImageIntersectsRectSelection(image, anchorPage, currentPage);
            } else {
                hit = (pagePolygon.size() < 3U)
                    ? ImageHitsPoint(image, currentPage)
                    : ImageIntersectsPolygonSelection(image, pagePolygon);
            }
            if (hit && pageView.pageIndex < selectedImageIndices_.size()) {
                selectedImageIndices_[pageView.pageIndex].push_back(imageIndex);
            }
        }

        for (std::size_t textIndex = 0; textIndex < page.texts.size(); ++textIndex) {
            const PageText& text = page.texts[textIndex];
            bool hit = false;
            if (effectiveSelectionMode == SelectionMode::Box) {
                hit = clickSelection
                    ? TextHitsPoint(text, page.width, currentPage)
                    : TextIntersectsRectSelection(text, page.width, anchorPage, currentPage);
            } else {
                hit = (pagePolygon.size() < 3U)
                    ? TextHitsPoint(text, page.width, currentPage)
                    : TextIntersectsPolygonSelection(text, page.width, pagePolygon);
            }
            if (hit && pageView.pageIndex < selectedTextIndices_.size()) {
                selectedTextIndices_[pageView.pageIndex].push_back(textIndex);
            }
        }
    }

    isSelecting_ = false;
    selectionLassoCursorPoints_.clear();
    selectionBoxOverride_ = false;
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::BeginImageTransform(std::size_t pageIndex, std::size_t imageIndex, ImageTransformHandle handle, POINT cursor) {
    if (pageIndex >= document_.PageCount() || imageIndex >= document_.PageAt(pageIndex).images.size()) {
        return;
    }

    isTransformingImage_ = true;
    transformingImagePageIndex_ = pageIndex;
    transformingImageIndex_ = imageIndex;
    imageTransformHandle_ = handle;
    imageTransformInitial_ = document_.PageAt(pageIndex).images[imageIndex];
    imageTransformStartPagePoint_ = ScreenToPage(cursor, nullptr);
    const Vec2 center = ImageCenter(imageTransformInitial_);
    imageTransformStartAngleDegrees_ = std::atan2(
        imageTransformStartPagePoint_.y - center.y,
        imageTransformStartPagePoint_.x - center.x) * (180.0f / kPi);
    interactionPreviewMode_ = InteractionPreviewMode::ImageTransform;
    interactionPreviewStartCursor_ = cursor;
    EnsureInteractionPreviewStorage();
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    BuildInteractionPageSnapshot(CalculatePageView(clientRect, pageIndex));
    RebuildImageTransformPreview(cursor);
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::RefreshTextEditBackgroundBrush(const RECT& editRect) {
    ReleaseTextEditBackgroundBrush();
    if (window_ == nullptr
        || !editingTextPageIndex_.has_value()
        || *editingTextPageIndex_ >= document_.PageCount()
        || editRect.right <= editRect.left
        || editRect.bottom <= editRect.top) {
        return;
    }

    HDC windowDc = GetDC(window_);
    if (windowDc == nullptr) {
        return;
    }

    HDC backgroundDc = CreateCompatibleDC(windowDc);
    if (backgroundDc == nullptr) {
        ReleaseDC(window_, windowDc);
        return;
    }

    const int width = editRect.right - editRect.left;
    const int height = editRect.bottom - editRect.top;
    void* bits = nullptr;
    HBITMAP bitmap = CreateTopDown32BitDibSection(backgroundDc, width, height, &bits);
    if (bitmap == nullptr || bits == nullptr) {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        DeleteDC(backgroundDc);
        ReleaseDC(window_, windowDc);
        return;
    }

    HGDIOBJ previousBitmap = SelectObject(backgroundDc, bitmap);
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT canvasRect = CanvasRect(clientRect);
    HBRUSH canvasBrush = CreateSolidBrush(RGB(22, 26, 33));
    FillRect(backgroundDc, &canvasRect, canvasBrush);
    DeleteObject(canvasBrush);

    const PageView pageView = CalculatePageView(clientRect, *editingTextPageIndex_);
    SetViewportOrgEx(backgroundDc, -editRect.left, -editRect.top, nullptr);
    if (pageView.valid) {
        PaintPage(backgroundDc, pageView);
    }
    SetViewportOrgEx(backgroundDc, 0, 0, nullptr);

    HBRUSH brush = CreatePatternBrush(bitmap);
    SelectObject(backgroundDc, previousBitmap);
    DeleteDC(backgroundDc);
    ReleaseDC(window_, windowDc);

    if (brush == nullptr) {
        DeleteObject(bitmap);
        return;
    }

    textEditBackgroundBitmap_ = bitmap;
    textEditBackgroundBrush_ = brush;
}

void App::CommitTextEdit() {
    if (textEditWindow_ == nullptr || !editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || !editingTextBeforePage_.has_value()) {
        return;
    }

    const std::size_t pageIndex = *editingTextPageIndex_;
    const std::size_t textIndex = *editingTextIndex_;
    Page beforePage = *editingTextBeforePage_;

    if (pageIndex < document_.PageCount() && textIndex < document_.PageAt(pageIndex).texts.size()) {
        const int textLength = GetWindowTextLengthW(textEditWindow_);
        std::wstring content(static_cast<std::size_t>(textLength), L'\0');
        if (textLength > 0) {
            std::vector<wchar_t> buffer(static_cast<std::size_t>(textLength) + 1U, L'\0');
            GetWindowTextW(textEditWindow_, buffer.data(), textLength + 1);
            content.assign(buffer.data(), static_cast<std::size_t>(textLength));
        }
        Page& page = document_.PageAt(pageIndex);
        page.texts[textIndex].content = std::move(content);
        if (page.texts[textIndex].content.empty()) {
            page.texts.erase(page.texts.begin() + static_cast<std::ptrdiff_t>(textIndex));
        }
    }

    EndTextEditControl();
    editingTextPageIndex_.reset();
    editingTextIndex_.reset();
    editingTextBeforePage_.reset();
    ClearSelection();
    if (pageIndex < document_.PageCount() && textIndex < document_.PageAt(pageIndex).texts.size()) {
        selectedTextIndices_[pageIndex].push_back(textIndex);
    }
    if (pageIndex < document_.PageCount()) {
        const bool changed = !PageEqual(beforePage, document_.PageAt(pageIndex));
        PushReplacePagesHistoryAction({ pageIndex }, { beforePage }, document_.CurrentPageIndex());
        if (changed) {
            NotifyDocumentMutated();
        }
        InvalidatePageSurfaceCache(pageIndex);
    }
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::BeginTextTransform(std::size_t pageIndex, std::size_t textIndex, ImageTransformHandle handle, POINT cursor) {
    if (pageIndex >= document_.PageCount() || textIndex >= document_.PageAt(pageIndex).texts.size()) {
        return;
    }

    pendingTextEditClick_ = false;
    pendingTextEditPageIndex_.reset();
    pendingTextEditIndex_.reset();
    isTransformingText_ = true;
    transformingTextPageIndex_ = pageIndex;
    transformingTextIndex_ = textIndex;
    textTransformHandle_ = handle;
    textTransformInitial_ = document_.PageAt(pageIndex).texts[textIndex];
    textTransformBeforePage_ = document_.PageAt(pageIndex);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, pageIndex);
    textTransformStartPagePoint_ = pageView.valid ? ScreenToPage(pageView, cursor, false) : ScreenToPage(cursor, nullptr);
    interactionPreviewMode_ = InteractionPreviewMode::TextTransform;
    interactionPreviewStartCursor_ = cursor;
    EnsureInteractionPreviewStorage();
    BuildInteractionPageSnapshot(pageView);
    UpdateTextTransform(cursor);
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::UpdateTextTransform(POINT cursor) {
    if (!isTransformingText_ || !transformingTextPageIndex_.has_value() || !transformingTextIndex_.has_value()) {
        return;
    }

    const std::size_t pageIndex = *transformingTextPageIndex_;
    const std::size_t textIndex = *transformingTextIndex_;
    if (pageIndex >= document_.PageCount() || textIndex >= document_.PageAt(pageIndex).texts.size()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, pageIndex);
    if (!pageView.valid) {
        return;
    }

    std::vector<std::optional<RECT>> previousBounds(document_.PageCount());
    for (std::size_t index = 0; index < document_.PageCount(); ++index) {
        previousBounds[index] = InteractionPreviewBoundsForPage(index);
    }

    const Page& page = document_.PageAt(pageIndex);
    const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const Vec2 currentPoint = ScreenToPage(pageView, cursor, false);
    const PageTextBounds initialBounds = MeasurePageTextBounds(textTransformInitial_, page.width);
    const float initialWidth = std::max(1.0f, initialBounds.right - initialBounds.left);
    const float initialHeight = std::max(1.0f, initialBounds.bottom - initialBounds.top);
    const float centerX = (initialBounds.left + initialBounds.right) * 0.5f;
    const float centerY = (initialBounds.top + initialBounds.bottom) * 0.5f;
    constexpr float kMinTextFontSize = 1.0f;
    const float minTextBoxWidth = std::max(4.0f, textTransformInitial_.fontSize * 0.2f);
    const float minTextBoxHeight = std::max(4.0f, textTransformInitial_.fontSize * 0.2f);
    const float minScale = std::max(0.05f, kMinTextFontSize / std::max(1.0f, textTransformInitial_.fontSize));
    const float maxScaleToFit = std::max(0.05f, std::min(page.width / initialWidth, page.height / initialHeight));

    auto clampScale = [&](float value) {
        return std::clamp(value, minScale, std::max(minScale, maxScaleToFit));
    };

    PageText updated = textTransformInitial_;
    if (shiftPressed) {
        float scale = 1.0f;
        float newLeft = initialBounds.left;
        float newTop = initialBounds.top;
        switch (textTransformHandle_) {
        case ImageTransformHandle::ResizeNorthWest: {
            scale = clampScale(std::max(
                (initialBounds.right - currentPoint.x) / initialWidth,
                (initialBounds.bottom - currentPoint.y) / initialHeight));
            newLeft = initialBounds.right - (initialWidth * scale);
            newTop = initialBounds.bottom - (initialHeight * scale);
            break;
        }
        case ImageTransformHandle::ResizeNorth: {
            scale = clampScale((initialBounds.bottom - currentPoint.y) / initialHeight);
            newLeft = centerX - ((initialWidth * scale) * 0.5f);
            newTop = initialBounds.bottom - (initialHeight * scale);
            break;
        }
        case ImageTransformHandle::ResizeNorthEast: {
            scale = clampScale(std::max(
                (currentPoint.x - initialBounds.left) / initialWidth,
                (initialBounds.bottom - currentPoint.y) / initialHeight));
            newLeft = initialBounds.left;
            newTop = initialBounds.bottom - (initialHeight * scale);
            break;
        }
        case ImageTransformHandle::ResizeEast: {
            scale = clampScale((currentPoint.x - initialBounds.left) / initialWidth);
            newLeft = initialBounds.left;
            newTop = centerY - ((initialHeight * scale) * 0.5f);
            break;
        }
        case ImageTransformHandle::ResizeSouthEast: {
            scale = clampScale(std::max(
                (currentPoint.x - initialBounds.left) / initialWidth,
                (currentPoint.y - initialBounds.top) / initialHeight));
            newLeft = initialBounds.left;
            newTop = initialBounds.top;
            break;
        }
        case ImageTransformHandle::ResizeSouth: {
            scale = clampScale((currentPoint.y - initialBounds.top) / initialHeight);
            newLeft = centerX - ((initialWidth * scale) * 0.5f);
            newTop = initialBounds.top;
            break;
        }
        case ImageTransformHandle::ResizeSouthWest: {
            scale = clampScale(std::max(
                (initialBounds.right - currentPoint.x) / initialWidth,
                (currentPoint.y - initialBounds.top) / initialHeight));
            newLeft = initialBounds.right - (initialWidth * scale);
            newTop = initialBounds.top;
            break;
        }
        case ImageTransformHandle::ResizeWest: {
            scale = clampScale((initialBounds.right - currentPoint.x) / initialWidth);
            newLeft = initialBounds.right - (initialWidth * scale);
            newTop = centerY - ((initialHeight * scale) * 0.5f);
            break;
        }
        default:
            return;
        }

        updated.fontSize = std::max(kMinTextFontSize, textTransformInitial_.fontSize * scale);
        if (textTransformInitial_.width > 0.0f) {
            updated.width = std::max(minTextBoxWidth, textTransformInitial_.width * scale);
        }
        if (textTransformInitial_.height > 0.0f) {
            updated.height = std::max(minTextBoxHeight, textTransformInitial_.height * scale);
        }
        updated.x = newLeft;
        updated.y = newTop;
    } else {
        float newLeft = initialBounds.left;
        float newTop = initialBounds.top;
        float newWidth = initialWidth;
        float newHeight = initialHeight;
        switch (textTransformHandle_) {
        case ImageTransformHandle::ResizeNorthWest:
        case ImageTransformHandle::ResizeSouthWest:
        case ImageTransformHandle::ResizeWest:
            newLeft = std::min(currentPoint.x, initialBounds.right - minTextBoxWidth);
            newWidth = initialBounds.right - newLeft;
            break;
        case ImageTransformHandle::ResizeNorthEast:
        case ImageTransformHandle::ResizeSouthEast:
        case ImageTransformHandle::ResizeEast:
            newLeft = initialBounds.left;
            newWidth = currentPoint.x - initialBounds.left;
            break;
        case ImageTransformHandle::ResizeNorth:
            newTop = std::min(currentPoint.y, initialBounds.bottom - minTextBoxHeight);
            newHeight = initialBounds.bottom - newTop;
            break;
        case ImageTransformHandle::ResizeSouth:
            newTop = initialBounds.top;
            newHeight = currentPoint.y - initialBounds.top;
            break;
        default:
            break;
        }

        switch (textTransformHandle_) {
        case ImageTransformHandle::ResizeNorthWest:
        case ImageTransformHandle::ResizeNorthEast:
            newTop = std::min(currentPoint.y, initialBounds.bottom - minTextBoxHeight);
            newHeight = initialBounds.bottom - newTop;
            break;
        case ImageTransformHandle::ResizeSouthEast:
        case ImageTransformHandle::ResizeSouthWest:
            newTop = initialBounds.top;
            newHeight = currentPoint.y - initialBounds.top;
            break;
        case ImageTransformHandle::ResizeEast:
        case ImageTransformHandle::ResizeWest:
            newTop = initialBounds.top;
            newHeight = initialHeight;
            break;
        case ImageTransformHandle::ResizeNorth:
        case ImageTransformHandle::ResizeSouth:
            newLeft = initialBounds.left;
            newWidth = initialWidth;
            break;
        default:
            break;
        }

        switch (textTransformHandle_) {
        case ImageTransformHandle::ResizeNorth:
        case ImageTransformHandle::ResizeSouth:
            break;
        case ImageTransformHandle::ResizeNorthWest:
        case ImageTransformHandle::ResizeSouthWest:
        case ImageTransformHandle::ResizeWest:
        case ImageTransformHandle::ResizeNorthEast:
        case ImageTransformHandle::ResizeSouthEast:
        case ImageTransformHandle::ResizeEast:
            break;
        default:
            newWidth = std::abs(currentPoint.x - centerX) * 2.0f;
            newLeft = centerX - (newWidth * 0.5f);
            break;
        }

        newWidth = std::max(minTextBoxWidth, newWidth);
        newHeight = std::max(minTextBoxHeight, newHeight);

        updated.x = newLeft;
        updated.y = newTop;
        updated.width = newWidth;
        updated.height = newHeight;
    }

    if (updated.width > 0.0f) {
        updated.width = std::max(1.0f, updated.width);
    }
    if (updated.height > 0.0f) {
        updated.height = std::max(1.0f, updated.height);
    }

    const PageTextBounds updatedBounds = MeasurePageTextBounds(updated, page.width);
    float deltaX = 0.0f;
    float deltaY = 0.0f;
    if (updatedBounds.top < 0.0f) {
        deltaY = -updatedBounds.top;
    } else if (updatedBounds.bottom > page.height) {
        deltaY = page.height - updatedBounds.bottom;
    }
    updated.x += deltaX;
    updated.y += deltaY;
    if (updated.width > 0.0f) {
        updated.width = std::max(1.0f, updated.width);
    }
    if (updated.height > 0.0f) {
        updated.height = std::max(1.0f, updated.height);
    }

    const PageTextBounds finalBounds = MeasurePageTextBounds(updated, page.width);
    if (interactionPreviewTexts_.size() != document_.PageCount()) {
        EnsureInteractionPreviewStorage();
    }
    interactionPreviewTexts_.assign(document_.PageCount(), {});
    interactionPreviewTexts_[pageIndex].push_back({
        pageIndex,
        textIndex,
        std::move(updated),
        std::max(1.0f, finalBounds.right - finalBounds.left),
        std::max(1.0f, finalBounds.bottom - finalBounds.top),
    });
    InvalidateInteractionPreviewDelta(previousBounds);
}

void App::EndTextTransform() {
    if (!isTransformingText_ || !transformingTextPageIndex_.has_value()) {
        return;
    }

    const std::size_t pageIndex = *transformingTextPageIndex_;
    if (pageIndex < document_.PageCount()
        && transformingTextIndex_.has_value()
        && *transformingTextIndex_ < document_.PageAt(pageIndex).texts.size()
        && pageIndex < interactionPreviewTexts_.size()) {
        for (const InteractionPreviewText& preview : interactionPreviewTexts_[pageIndex]) {
            if (preview.sourcePageIndex == pageIndex && preview.sourceIndex == *transformingTextIndex_) {
                document_.PageAt(pageIndex).texts[*transformingTextIndex_] = preview.text;
                break;
            }
        }
    }
    const bool changed = pageIndex < document_.PageCount() && !PageEqual(textTransformBeforePage_, document_.PageAt(pageIndex));

    isTransformingText_ = false;
    transformingTextPageIndex_.reset();
    transformingTextIndex_.reset();
    textTransformHandle_ = ImageTransformHandle::None;
    ClearInteractionPreview();
    UpdateCaptureState();

    if (pageIndex < document_.PageCount()) {
        PushReplacePagesHistoryAction({ pageIndex }, { textTransformBeforePage_ }, document_.CurrentPageIndex());
        if (changed) {
            NotifyDocumentMutated();
        }
        InvalidatePageSurfaceCache(pageIndex);
    }

    InvalidateCanvas();
}

void App::BeginStrokeTransform(std::size_t pageIndex, ImageTransformHandle handle, POINT cursor) {
    if (pageIndex >= document_.PageCount() || pageIndex >= selectedStrokeIndices_.size() || selectedStrokeIndices_[pageIndex].empty()) {
        return;
    }

    isTransformingStroke_ = true;
    transformingStrokePageIndex_ = pageIndex;
    strokeTransformHandle_ = handle;
    strokeTransformStartPagePoint_ = ScreenToPage(cursor, nullptr);
    strokeTransformInitials_.clear();

    const Page& page = document_.PageAt(pageIndex);
    float minX = page.width;
    float minY = page.height;
    float maxX = 0.0f;
    float maxY = 0.0f;
    for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
        if (strokeIndex >= page.strokes.size()) {
            continue;
        }
        strokeTransformInitials_.push_back({ strokeIndex, page.strokes[strokeIndex] });
        for (const Vec2& point : page.strokes[strokeIndex].points) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
    }

    strokeTransformBoundsMin_ = { minX, minY };
    strokeTransformBoundsMax_ = { maxX, maxY };
    const Vec2 center {
        (minX + maxX) * 0.5f,
        (minY + maxY) * 0.5f,
    };
    strokeTransformStartAngleDegrees_ = std::atan2(
        strokeTransformStartPagePoint_.y - center.y,
        strokeTransformStartPagePoint_.x - center.x) * (180.0f / kPi);
    interactionPreviewMode_ = InteractionPreviewMode::StrokeTransform;
    interactionPreviewStartCursor_ = cursor;
    EnsureInteractionPreviewStorage();
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    BuildInteractionPageSnapshot(CalculatePageView(clientRect, pageIndex));
    RebuildStrokeTransformPreview(cursor);
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::BeginMoveSelection(POINT cursor) {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        if (HasSelection()) {
            ClearSelection();
            InvalidateCanvas();
        }
        return;
    }

    const std::size_t pageIndex = hitPage->pageIndex;
    const Vec2 pagePoint = ScreenToPage(cursor, nullptr);
    const float hitRadius = std::max(8.0f, brushWidth_ * 2.5f) / std::max(0.1f, hitPage->scale);
    const Page& page = document_.PageAt(pageIndex);
    std::size_t strokeHandlePageIndex = 0;
    const auto strokeHandle = HitTestSelectedStrokeHandle(cursor, &strokeHandlePageIndex);
    if (strokeHandle.has_value() && strokeHandlePageIndex == pageIndex) {
        BeginStrokeTransform(pageIndex, *strokeHandle, cursor);
        return;
    }

    std::size_t imageHandlePageIndex = 0;
    std::size_t imageHandleImageIndex = 0;
    const auto imageHandle = HitTestSelectedImageHandle(cursor, &imageHandlePageIndex, &imageHandleImageIndex);
    if (imageHandle.has_value() && imageHandlePageIndex == pageIndex) {
        BeginImageTransform(pageIndex, imageHandleImageIndex, *imageHandle, cursor);
        return;
    }

    std::size_t textHandlePageIndex = 0;
    std::size_t textHandleTextIndex = 0;
    const auto textHandle = HitTestSelectedTextHandle(cursor, &textHandlePageIndex, &textHandleTextIndex);
    if (textHandle.has_value() && textHandlePageIndex == pageIndex) {
        BeginTextTransform(pageIndex, textHandleTextIndex, *textHandle, cursor);
        return;
    }

    const bool clickedSelectionMoveGrip = CursorHitsSelectionMoveGrip(cursor);
    std::optional<std::size_t> clickedSelectedStroke {};
    std::optional<std::size_t> clickedSelectedText {};
    std::optional<std::size_t> clickedSelectedImage {};
    std::optional<std::size_t> clickedStroke {};
    std::optional<std::size_t> clickedText {};
    std::optional<std::size_t> clickedImage {};

    if (!clickedSelectionMoveGrip && pageIndex < selectedStrokeIndices_.size()) {
        for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
            if (strokeIndex < page.strokes.size() && StrokeHitsPoint(page.strokes[strokeIndex], pagePoint, hitRadius, settings_.strokeSmoothing)) {
                clickedSelectedStroke = strokeIndex;
                break;
            }
        }
    }
    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && pageIndex < selectedTextIndices_.size()) {
        for (std::size_t textIndex : selectedTextIndices_[pageIndex]) {
            if (textIndex < page.texts.size() && TextHitsPoint(page.texts[textIndex], page.width, pagePoint)) {
                clickedSelectedText = textIndex;
                break;
            }
        }
    }
    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value() && pageIndex < selectedImageIndices_.size()) {
        for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
            if (imageIndex < page.images.size() && ImageHitsPoint(page.images[imageIndex], pagePoint)) {
                clickedSelectedImage = imageIndex;
                break;
            }
        }
    }

    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value() && !clickedSelectedImage.has_value()) {
        for (std::size_t strokeIndex = page.strokes.size(); strokeIndex > 0; --strokeIndex) {
            const std::size_t actualIndex = strokeIndex - 1U;
            if (StrokeHitsPoint(page.strokes[actualIndex], pagePoint, hitRadius, settings_.strokeSmoothing)) {
                clickedStroke = actualIndex;
                break;
            }
        }
    }
    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value() && !clickedSelectedImage.has_value() && !clickedStroke.has_value()) {
        for (std::size_t textIndex = page.texts.size(); textIndex > 0; --textIndex) {
            const std::size_t actualIndex = textIndex - 1U;
            if (TextHitsPoint(page.texts[actualIndex], page.width, pagePoint)) {
                clickedText = actualIndex;
                break;
            }
        }
    }
    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value() && !clickedSelectedImage.has_value()
        && !clickedStroke.has_value() && !clickedText.has_value()) {
        for (std::size_t imageIndex = page.images.size(); imageIndex > 0; --imageIndex) {
            const std::size_t actualIndex = imageIndex - 1U;
            if (ImageHitsPoint(page.images[actualIndex], pagePoint)) {
                clickedImage = actualIndex;
                break;
            }
        }
    }

    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value() && !clickedSelectedImage.has_value()
        && !clickedStroke.has_value() && !clickedText.has_value() && !clickedImage.has_value()) {
        ClearSelection();
        BeginSelection(cursor, true);
        return;
    }

    if (!clickedSelectionMoveGrip && !clickedSelectedStroke.has_value() && !clickedSelectedText.has_value() && !clickedSelectedImage.has_value()) {
        ClearSelection();
        if (clickedStroke.has_value() && pageIndex < selectedStrokeIndices_.size()) {
            selectedStrokeIndices_[pageIndex].push_back(*clickedStroke);
        }
        if (clickedText.has_value() && pageIndex < selectedTextIndices_.size()) {
            selectedTextIndices_[pageIndex].push_back(*clickedText);
        }
        if (clickedImage.has_value() && pageIndex < selectedImageIndices_.size()) {
            selectedImageIndices_[pageIndex].push_back(*clickedImage);
        }
    }

    if (!clickedSelectionMoveGrip && activeTool_ == Tool::Move && clickedSelectedText.has_value() && !clickedSelectedStroke.has_value() && !clickedSelectedImage.has_value()) {
        pendingTextEditClick_ = true;
        pendingTextEditCursor_ = cursor;
        pendingTextEditPageIndex_ = pageIndex;
        pendingTextEditIndex_ = *clickedSelectedText;
        UpdateCaptureState();
        return;
    }

    pendingTextEditClick_ = false;
    pendingTextEditPageIndex_.reset();
    pendingTextEditIndex_.reset();
    isMovingSelection_ = true;
    moveSelectionPageIndex_ = pageIndex;
    interactionPreviewMode_ = InteractionPreviewMode::MoveSelection;
    interactionPreviewStartCursor_ = cursor;
    lastMoveCursor_ = cursor;
    moveSelectionScale_ = std::max(0.1f, hitPage->scale);
    EnsureInteractionPreviewStorage();
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const std::vector<PageView> pageViews = CalculatePageViews(clientRect);
    for (const PageView& previewPageView : pageViews) {
        const bool hasSelectedStrokes = previewPageView.pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[previewPageView.pageIndex].empty();
        const bool hasSelectedTexts = previewPageView.pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[previewPageView.pageIndex].empty();
        const bool hasSelectedImages = previewPageView.pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[previewPageView.pageIndex].empty();
        if (hasSelectedStrokes || hasSelectedTexts || hasSelectedImages) {
            BuildInteractionPageSnapshot(previewPageView);
        }
    }
    RebuildMoveSelectionTextCache();
    RebuildMoveSelectionPreview(cursor);
    document_.SetCurrentPageIndex(pageIndex);
    MarkPdfSidebarCompositionDirty();
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::UpdateImageTransform(POINT cursor) {
    if (!isTransformingImage_ || !transformingImagePageIndex_.has_value() || !transformingImageIndex_.has_value()) {
        return;
    }
    RebuildImageTransformPreview(cursor);
}

void App::UpdateStrokeTransform(POINT cursor) {
    if (!isTransformingStroke_ || !transformingStrokePageIndex_.has_value()) {
        return;
    }
    RebuildStrokeTransformPreview(cursor);
}

void App::EndStrokeTransform() {
    if (!isTransformingStroke_) {
        return;
    }

    std::vector<std::size_t> affectedPageIndices;
    std::vector<Page> beforePages;
    if (transformingStrokePageIndex_.has_value() && *transformingStrokePageIndex_ < document_.PageCount()) {
        affectedPageIndices.push_back(*transformingStrokePageIndex_);
        beforePages.push_back(document_.PageAt(*transformingStrokePageIndex_));
    }
    const std::size_t focusPageIndex = std::min(document_.CurrentPageIndex(), document_.PageCount() - 1U);

    CommitStrokeTransformPreview();
    PushReplacePagesHistoryAction(std::move(affectedPageIndices), std::move(beforePages), focusPageIndex);
    ClearInteractionPreview();
    isTransformingStroke_ = false;
    transformingStrokePageIndex_.reset();
    strokeTransformHandle_ = ImageTransformHandle::None;
    strokeTransformInitials_.clear();
    NotifyDocumentMutated();
    InvalidateCanvas();
    UpdateCaptureState();
}

void App::UpdateMoveSelection(POINT cursor) {
    if (pendingTextEditClick_) {
        if (DistanceSquared(PointToVec2(cursor), PointToVec2(pendingTextEditCursor_)) < (5.0f * 5.0f)
            || !pendingTextEditPageIndex_.has_value()) {
            return;
        }

        pendingTextEditClick_ = false;
        const std::size_t pageIndex = *pendingTextEditPageIndex_;
        pendingTextEditPageIndex_.reset();
        pendingTextEditIndex_.reset();

        const auto hitPage = HitTestPage(pendingTextEditCursor_);
        if (!hitPage.has_value() || pageIndex >= document_.PageCount()) {
            UpdateCaptureState();
            return;
        }

        isMovingSelection_ = true;
        moveSelectionPageIndex_ = pageIndex;
        interactionPreviewMode_ = InteractionPreviewMode::MoveSelection;
        interactionPreviewStartCursor_ = pendingTextEditCursor_;
        lastMoveCursor_ = pendingTextEditCursor_;
        moveSelectionScale_ = std::max(0.1f, hitPage->scale);
        EnsureInteractionPreviewStorage();
        RECT clientRect {};
        GetClientRect(window_, &clientRect);
        const std::vector<PageView> pageViews = CalculatePageViews(clientRect);
        for (const PageView& previewPageView : pageViews) {
            const bool hasSelectedStrokes = previewPageView.pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[previewPageView.pageIndex].empty();
            const bool hasSelectedTexts = previewPageView.pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[previewPageView.pageIndex].empty();
            const bool hasSelectedImages = previewPageView.pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[previewPageView.pageIndex].empty();
            if (hasSelectedStrokes || hasSelectedTexts || hasSelectedImages) {
                BuildInteractionPageSnapshot(previewPageView);
            }
        }
        RebuildMoveSelectionTextCache();
        RebuildMoveSelectionPreview(cursor);
        document_.SetCurrentPageIndex(pageIndex);
        MarkPdfSidebarCompositionDirty();
        UpdateCaptureState();
        InvalidateCanvas();
        return;
    }

    if (!isMovingSelection_) {
        return;
    }

    if (cursor.x == lastMoveCursor_.x && cursor.y == lastMoveCursor_.y) {
        return;
    }
    RebuildMoveSelectionPreview(cursor);
    lastMoveCursor_ = cursor;
}

void App::EndImageTransform() {
    if (!isTransformingImage_) {
        return;
    }

    std::vector<std::size_t> affectedPageIndices;
    std::vector<Page> beforePages;
    if (transformingImagePageIndex_.has_value() && *transformingImagePageIndex_ < document_.PageCount()) {
        affectedPageIndices.push_back(*transformingImagePageIndex_);
        beforePages.push_back(document_.PageAt(*transformingImagePageIndex_));
    }
    const std::size_t focusPageIndex = std::min(document_.CurrentPageIndex(), document_.PageCount() - 1U);

    CommitImageTransformPreview();
    PushReplacePagesHistoryAction(std::move(affectedPageIndices), std::move(beforePages), focusPageIndex);
    ClearInteractionPreview();
    isTransformingImage_ = false;
    transformingImagePageIndex_.reset();
    transformingImageIndex_.reset();
    imageTransformHandle_ = ImageTransformHandle::None;
    NotifyDocumentMutated();
    InvalidateCanvas();
    UpdateCaptureState();
}

void App::EndMoveSelection() {
    if (!isMovingSelection_) {
        return;
    }

    if (lastMoveCursor_.x == interactionPreviewStartCursor_.x && lastMoveCursor_.y == interactionPreviewStartCursor_.y) {
        ClearInteractionPreview();
        isMovingSelection_ = false;
        moveSelectionPageIndex_.reset();
        InvalidateCanvas();
        UpdateCaptureState();
        return;
    }

    std::vector<std::size_t> affectedPageIndices;
    affectedPageIndices.reserve(document_.PageCount());
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        const bool hasSelectedStrokes = pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty();
        const bool hasSelectedTexts = pageIndex < selectedTextIndices_.size() && !selectedTextIndices_[pageIndex].empty();
        const bool hasSelectedImages = pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty();
        const bool hasPreviewStrokes = pageIndex < interactionPreviewStrokes_.size() && !interactionPreviewStrokes_[pageIndex].empty();
        const bool hasPreviewTexts = pageIndex < interactionPreviewTexts_.size() && !interactionPreviewTexts_[pageIndex].empty();
        const bool hasPreviewImages = pageIndex < interactionPreviewImages_.size() && !interactionPreviewImages_[pageIndex].empty();
        if (hasSelectedStrokes || hasSelectedTexts || hasSelectedImages || hasPreviewStrokes || hasPreviewTexts || hasPreviewImages) {
            affectedPageIndices.push_back(pageIndex);
        }
    }

    std::vector<Page> beforePages;
    beforePages.reserve(affectedPageIndices.size());
    for (std::size_t pageIndex : affectedPageIndices) {
        beforePages.push_back(document_.PageAt(pageIndex));
    }
    const std::size_t focusPageIndex = std::min(document_.CurrentPageIndex(), document_.PageCount() - 1U);

    CommitMoveSelectionPreview();
    PushReplacePagesHistoryAction(std::move(affectedPageIndices), std::move(beforePages), focusPageIndex);
    ClearInteractionPreview();
    isMovingSelection_ = false;
    moveSelectionPageIndex_.reset();
    NotifyDocumentMutated();
    InvalidateCanvas();
    UpdateCaptureState();
}

void App::BeginErase(POINT cursor) {
    if (isPanning_) {
        return;
    }

    if (!HitTestPage(cursor).has_value()) {
        return;
    }

    if (HasSelection()) {
        ClearSelection();
        InvalidateCanvas();
    }
    isErasing_ = true;
    lastErasePageIndex_.reset();
    UpdateCaptureState();
    static_cast<void>(EraseAt(cursor));
}

void App::ExtendErase(POINT cursor) {
    if (!isErasing_) {
        return;
    }

    static_cast<void>(EraseAt(cursor));
}

void App::EnsurePendingEraseStorage() {
    if (pendingEraseStrokeIndices_.size() != document_.PageCount()) {
        pendingEraseStrokeIndices_.assign(document_.PageCount(), {});
    }
    if (pendingEraseImageIndices_.size() != document_.PageCount()) {
        pendingEraseImageIndices_.assign(document_.PageCount(), {});
    }
    if (pendingEraseTextIndices_.size() != document_.PageCount()) {
        pendingEraseTextIndices_.assign(document_.PageCount(), {});
    }
}

bool App::PageHasPendingErase(std::size_t pageIndex) const {
    return (pageIndex < pendingEraseStrokeIndices_.size() && !pendingEraseStrokeIndices_[pageIndex].empty())
        || (pageIndex < pendingEraseImageIndices_.size() && !pendingEraseImageIndices_[pageIndex].empty())
        || (pageIndex < pendingEraseTextIndices_.size() && !pendingEraseTextIndices_[pageIndex].empty());
}

bool App::IsStrokePendingErase(std::size_t pageIndex, std::size_t strokeIndex) const {
    if (pageIndex >= pendingEraseStrokeIndices_.size()) {
        return false;
    }

    const auto& pending = pendingEraseStrokeIndices_[pageIndex];
    return std::find(pending.begin(), pending.end(), strokeIndex) != pending.end();
}

bool App::IsImagePendingErase(std::size_t pageIndex, std::size_t imageIndex) const {
    if (pageIndex >= pendingEraseImageIndices_.size()) {
        return false;
    }

    const auto& pending = pendingEraseImageIndices_[pageIndex];
    return std::find(pending.begin(), pending.end(), imageIndex) != pending.end();
}

bool App::IsTextPendingErase(std::size_t pageIndex, std::size_t textIndex) const {
    if (pageIndex >= pendingEraseTextIndices_.size()) {
        return false;
    }

    const auto& pending = pendingEraseTextIndices_[pageIndex];
    return std::find(pending.begin(), pending.end(), textIndex) != pending.end();
}

void App::ClearPendingEraseObjects() {
    for (auto& pagePending : pendingEraseStrokeIndices_) {
        pagePending.clear();
    }
    for (auto& pagePending : pendingEraseImageIndices_) {
        pagePending.clear();
    }
    for (auto& pagePending : pendingEraseTextIndices_) {
        pagePending.clear();
    }
}

void App::EndErase() {
    isErasing_ = false;
    lastErasePageIndex_.reset();
    UpdateCaptureState();

    bool removedAny = false;
    std::vector<std::size_t> affectedPageIndices;
    std::vector<Page> beforePages;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        const bool hasPendingStrokes = pageIndex < pendingEraseStrokeIndices_.size() && !pendingEraseStrokeIndices_[pageIndex].empty();
        const bool hasPendingImages = pageIndex < pendingEraseImageIndices_.size() && !pendingEraseImageIndices_[pageIndex].empty();
        const bool hasPendingTexts = pageIndex < pendingEraseTextIndices_.size() && !pendingEraseTextIndices_[pageIndex].empty();
        if (!hasPendingStrokes && !hasPendingImages && !hasPendingTexts) {
            continue;
        }

        Page& page = document_.PageAt(pageIndex);
        affectedPageIndices.push_back(pageIndex);
        beforePages.push_back(page);
        bool pageRemovedAny = false;

        auto removeObjects = [&](auto& objects, std::vector<std::size_t>& pending) {
            std::sort(pending.begin(), pending.end());
            pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
            for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
                const std::size_t objectIndex = *it;
                if (objectIndex < objects.size()) {
                    objects.erase(objects.begin() + static_cast<std::ptrdiff_t>(objectIndex));
                    pageRemovedAny = true;
                    removedAny = true;
                }
            }
        };

        if (hasPendingTexts) {
            removeObjects(page.texts, pendingEraseTextIndices_[pageIndex]);
        }
        if (hasPendingImages) {
            removeObjects(page.images, pendingEraseImageIndices_[pageIndex]);
        }
        if (hasPendingStrokes) {
            auto& pending = pendingEraseStrokeIndices_[pageIndex];
            std::sort(pending.begin(), pending.end());
            pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
            for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
                const std::size_t strokeIndex = *it;
                if (strokeIndex >= page.strokes.size()) {
                    continue;
                }

                page.strokes.erase(page.strokes.begin() + static_cast<std::ptrdiff_t>(strokeIndex));
                AdjustSelectionForRemoval(pageIndex, strokeIndex);
                removedAny = true;
                pageRemovedAny = true;
            }
        }

        if (pageRemovedAny) {
            InvalidatePageSurfaceCache(pageIndex);
        }
    }

    ClearPendingEraseObjects();
    if (removedAny) {
        PushReplacePagesHistoryAction(std::move(affectedPageIndices), std::move(beforePages), document_.CurrentPageIndex());
        NotifyDocumentMutated();
    }
    InvalidateCanvas();
}

bool App::EraseAt(POINT cursor) {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        lastErasePageIndex_.reset();
        return false;
    }

    const PageView pageView = *hitPage;
    if (!pageView.valid || pageView.scale <= 0.0f) {
        lastErasePageIndex_.reset();
        return false;
    }

    const Vec2 center = ScreenToPage(pageView, cursor, true);
    const Vec2 eraseStart = lastErasePageIndex_.has_value() && *lastErasePageIndex_ == pageView.pageIndex
        ? lastErasePagePoint_
        : center;
    const float eraseRadiusPage = EraserRadiusPixels() / pageView.scale;
    const float eraseRadiusSquared = eraseRadiusPage * eraseRadiusPage;
    Page& page = document_.PageAt(pageView.pageIndex);
    if (document_.SetCurrentPageIndex(pageView.pageIndex)) {
        MarkPdfSidebarCompositionDirty();
    }
    EnsurePendingEraseStorage();

    bool hitAny = false;
    std::optional<RECT> dirtyPageRect;
    const auto addDirtyRect = [&](std::optional<RECT> bounds) {
        if (!bounds.has_value()) {
            return;
        }
        dirtyPageRect = dirtyPageRect.has_value()
            ? UnionRectangles(*dirtyPageRect, *bounds)
            : bounds;
    };
    for (std::size_t imageIndex = 0; imageIndex < page.images.size(); ++imageIndex) {
        if (!IsImagePendingErase(pageView.pageIndex, imageIndex)
            && ImageHitsSegment(page.images[imageIndex], eraseStart, center, eraseRadiusPage)) {
            pendingEraseImageIndices_[pageView.pageIndex].push_back(imageIndex);
            addDirtyRect(ImageBoundsRect(page.images[imageIndex], pageView.scale, 8));
            hitAny = true;
        }
    }

    for (std::size_t textIndex = 0; textIndex < page.texts.size(); ++textIndex) {
        if (!IsTextPendingErase(pageView.pageIndex, textIndex)
            && TextHitsSegment(page.texts[textIndex], page.width, eraseStart, center, eraseRadiusPage)) {
            pendingEraseTextIndices_[pageView.pageIndex].push_back(textIndex);
            addDirtyRect(PageTextBoundsRect(page.texts[textIndex], page.width, pageView.scale, 8));
            hitAny = true;
        }
    }

    for (std::size_t strokeIndex = 0; strokeIndex < page.strokes.size(); ++strokeIndex) {
        if (IsStrokePendingErase(pageView.pageIndex, strokeIndex)) {
            continue;
        }

        const Stroke& stroke = page.strokes[strokeIndex];
        const std::vector<Vec2> strokePoints = StrokeRenderPoints(stroke, settings_.strokeSmoothing);
        if (strokePoints.empty()) {
            continue;
        }

        bool hit = DistancePointToSegmentSquared(strokePoints.front(), eraseStart, center) <= eraseRadiusSquared;
        for (std::size_t index = 1; !hit && index < strokePoints.size(); ++index) {
            hit = DistanceSegmentToSegmentSquared(eraseStart, center, strokePoints[index - 1], strokePoints[index]) <= eraseRadiusSquared;
        }

        if (hit) {
            pendingEraseStrokeIndices_[pageView.pageIndex].push_back(strokeIndex);
            addDirtyRect(StrokeRenderBoundsRect(stroke, pageView.scale, false, settings_.strokeSmoothing, 12));
            hitAny = true;
        }
    }

    lastErasePageIndex_ = pageView.pageIndex;
    lastErasePagePoint_ = center;

    if (hitAny) {
        RECT dirtyRect = dirtyPageRect.value_or(MakeRect(0, 0, pageView.rect.right - pageView.rect.left, pageView.rect.bottom - pageView.rect.top));
        OffsetRect(&dirtyRect, pageView.rect.left, pageView.rect.top);
        InvalidateRect(window_, &dirtyRect, FALSE);
    }

    return hitAny;
}

void App::BeginPan(POINT cursor, bool fromMiddleButton) {
    if (isDrawing_) {
        EndStroke();
    }

    if (!fromMiddleButton && HasSelection()) {
        ClearSelection();
    }
    isPanning_ = true;
    panFromMiddleButton_ = fromMiddleButton;
    lastPanCursor_ = cursor;
    UpdateCaptureState();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::UpdatePan(POINT cursor) {
    if (!isPanning_) {
        return;
    }

    const float dx = static_cast<float>(cursor.x - lastPanCursor_.x);
    const float dy = static_cast<float>(cursor.y - lastPanCursor_.y);
    lastPanCursor_ = cursor;
    PanBy(dx, dy);
}

void App::EndPan() {
    isPanning_ = false;
    panFromMiddleButton_ = false;
    UpdateCaptureState();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::UpdateCaptureState() {
    if (toolbarBrushSliderDragging_ || isDrawing_ || isPanning_ || isSelecting_ || pendingTextEditClick_
        || isMovingSelection_ || isTransformingImage_ || isTransformingText_ || isTransformingStroke_) {
        SetCapture(window_);
        return;
    }

    if (isErasing_) {
        SetCapture(window_);
        return;
    }

    if (GetCapture() == window_) {
        ReleaseCapture();
    }
}

void App::ExecuteCommand(unsigned int commandId) {
    if (commandId >= kCommandRecentDocumentBase) {
        const std::size_t recentIndex = static_cast<std::size_t>(commandId - kCommandRecentDocumentBase);
        if (recentIndex < recentDocumentPaths_.size()) {
            static_cast<void>(OpenRecentDocument(recentIndex));
            return;
        }
    }

    switch (commandId) {
    case kCommandFileNew:
        CreateBlankDocument();
        break;
    case kCommandFileOpen:
        OpenDocument();
        break;
    case kCommandFileSaveCnote:
        static_cast<void>(SaveCurrentCnote());
        break;
    case kCommandFileExportPdf:
        static_cast<void>(ExportAsPdf());
        break;
    case kCommandFileExit:
        PostMessageW(window_, WM_CLOSE, 0, 0);
        break;
    case kCommandEditBrushSettings:
        ToggleBrushSettings();
        break;
    case kCommandSelectLasso:
        selectionMode_ = SelectionMode::Lasso;
        SetActiveTool(Tool::Select);
        break;
    default:
        break;
    }
}

bool App::HandleToolbarClick(POINT cursor) {
    const UINT dpi = WindowDpi(window_);
    if (cursor.y >= VisibleToolbarHeight()) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const ToolbarLayout layout = BuildToolbarLayout(clientRect, palette_.size(), IsZoomed(window_), activeTool_, dpi);
    HDC windowDc = GetDC(window_);
    HFONT bodyFont = CreateFontW(FontHeightForDpi(15, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HGDIOBJ previousFont = windowDc != nullptr && bodyFont != nullptr ? SelectObject(windowDc, bodyFont) : nullptr;
    const ToolbarTitleLayout titleLayout = windowDc != nullptr
        ? ResolveToolbarTitleLayout(layout, windowDc, CurrentWindowTitle(), ZoomIndicatorLabel(), dpi)
        : ToolbarTitleLayout {};
    if (windowDc != nullptr && previousFont != nullptr) {
        SelectObject(windowDc, previousFont);
    }
    if (bodyFont != nullptr) {
        DeleteObject(bodyFont);
    }
    if (windowDc != nullptr) {
        ReleaseDC(window_, windowDc);
    }

    CancelPageJumpInput();
    CancelQuickSizePopup();
    if (toolbarZoomEditing_ && PtInRect(&titleLayout.zoomRect, cursor) == 0) {
        ApplyToolbarZoomInput();
        CancelToolbarZoomInput();
    }

    const auto openTitleMenu = [&](RECT rect) {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) {
            return;
        }

        HMENU openMenu = CreatePopupMenu();
        if (openMenu != nullptr) {
            AppendMenuW(openMenu, MF_STRING, kCommandFileOpen, L"Choose...");
            if (!recentDocumentPaths_.empty()) {
                AppendMenuW(openMenu, MF_SEPARATOR, 0, nullptr);
                for (std::size_t index = 0; index < recentDocumentPaths_.size(); ++index) {
                    const std::wstring label = RecentDocumentMenuLabel(recentDocumentPaths_[index]);
                    AppendMenuW(openMenu, MF_STRING, kCommandRecentDocumentBase + static_cast<unsigned int>(index), label.c_str());
                }
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(openMenu), L"Open");
        }
        AppendMenuW(menu, MF_STRING, kCommandFileNew, L"New");
        AppendMenuW(menu, MF_STRING, kCommandFileSaveCnote, HasSavedCnotePath() ? L"Save Cnote" : L"Save as Cnote...");
        AppendMenuW(menu, MF_STRING, kCommandFileExportPdf, L"Export as PDF...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandEditBrushSettings, IsSettingsWindowOpen() ? L"Hide Settings" : L"Settings");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandFileExit, L"Exit");

        titleMenuActive_ = true;
        RECT clientRectForMenu {};
        GetClientRect(window_, &clientRectForMenu);
        clientRectForMenu.bottom = FullToolbarHeight();
        InvalidateRect(window_, &clientRectForMenu, FALSE);
        UpdateWindow(window_);

        POINT anchor { rect.left, rect.bottom + 2 };
        ClientToScreen(window_, &anchor);
        const unsigned int selected = TrackPopupMenu(
            menu,
            TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
            anchor.x,
            anchor.y,
            0,
            window_,
            nullptr);
        DestroyMenu(menu);
        titleMenuActive_ = false;
        InvalidateRect(window_, &clientRectForMenu, FALSE);

        if (selected != 0U) {
            ExecuteCommand(selected);
        }
    };

    if (PtInRect(&layout.minimizeRect, cursor) != 0) {
        ClosePaletteOverlays();
        ShowWindow(window_, SW_MINIMIZE);
        return true;
    }

    if (PtInRect(&layout.maximizeRect, cursor) != 0) {
        ClosePaletteOverlays();
        ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE);
        return true;
    }

    if (PtInRect(&layout.closeRect, cursor) != 0) {
        ClosePaletteOverlays();
        PostMessageW(window_, WM_CLOSE, 0, 0);
        return true;
    }

    if (PtInRect(&titleLayout.fileTitleRect, cursor) != 0) {
        ClosePaletteOverlays();
        openTitleMenu(titleLayout.fileTitleRect);
        return true;
    }

    if (PtInRect(&titleLayout.zoomRect, cursor) != 0) {
        ClosePaletteOverlays();
        if (!toolbarZoomEditing_) {
            BeginToolbarZoomInput();
            InvalidateRect(window_, nullptr, FALSE);
        }
        return true;
    }

    if (PtInRect(&layout.drawToolRect, cursor) != 0) {
        ClosePaletteOverlays();
        SetActiveTool(Tool::Draw);
        return true;
    }

    if (PtInRect(&layout.selectToolRect, cursor) != 0) {
        ClosePaletteOverlays();
        selectionMode_ = SelectionMode::Lasso;
        SetActiveTool(Tool::Select);
        return true;
    }

    if (PtInRect(&layout.textToolRect, cursor) != 0) {
        ClosePaletteOverlays();
        SetActiveTool(Tool::Text);
        return true;
    }

    if (PtInRect(&layout.moveToolRect, cursor) != 0) {
        ClosePaletteOverlays();
        SetActiveTool(Tool::Move);
        return true;
    }

    if (PtInRect(&layout.panToolRect, cursor) != 0) {
        ClosePaletteOverlays();
        SetActiveTool(Tool::Pan);
        return true;
    }

    if (PtInRect(&layout.eraseToolRect, cursor) != 0) {
        ClosePaletteOverlays();
        SetActiveTool(Tool::Erase);
        return true;
    }

    if (PtInRect(&layout.undoRect, cursor) != 0) {
        ClosePaletteOverlays();
        Undo();
        return true;
    }

    if (PtInRect(&layout.redoRect, cursor) != 0) {
        ClosePaletteOverlays();
        Redo();
        return true;
    }

    return true;
}

bool App::HandleSidebarClick(POINT cursor) {
    if (!pdfSidebarOpen_ || !IsPdfSidebarVisible()) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT sidebarRect = PdfSidebarRect(clientRect);
    if (PtInRect(&sidebarRect, cursor) == 0) {
        return false;
    }

    const SidebarHeaderLayout headerLayout = BuildSidebarHeaderLayout(sidebarRect, WindowDpi(window_), IsZoomed(window_) != FALSE);
    const bool clickedPageLabel = PtInRect(&headerLayout.pageLabelRect, cursor) != 0;
    if (pageJumpEditing_ && !clickedPageLabel) {
        CancelPageJumpInput();
    }
    if (toolbarZoomEditing_) {
        ApplyToolbarZoomInput();
        CancelToolbarZoomInput();
    }

    CancelQuickSizePopup();

    if (clickedPageLabel) {
        BeginPageJumpInput();
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    if (PtInRect(&headerLayout.addPageRect, cursor) != 0) {
        AddBlankPageAfterCurrent();
        return true;
    }

    if (PtInRect(&headerLayout.newPageSettingsRect, cursor) != 0) {
        ToggleNewPageSettings();
        return true;
    }

    if (const auto sidebarPage = HitTestSidebarPage(cursor); sidebarPage.has_value()) {
        FocusPage(*sidebarPage);
    }
    return true;
}

bool App::HandleQuickSizePopupClick(POINT cursor) {
    if (!quickSizePopupTool_.has_value() || window_ == nullptr) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT canvasRect = CanvasRect(clientRect);
    const UINT dpi = WindowDpi(window_);
    const QuickSizePopupLayout popupLayout = BuildQuickSizePopupLayout(
        canvasRect,
        quickSizePopupAnchor_,
        dpi,
        QuickSizePreviewExtentForTool(*quickSizePopupTool_, clientRect));
    const bool clickedSlider = PtInRect(&popupLayout.sliderRect, cursor) != 0;
    const bool clickedValue = PtInRect(&popupLayout.valueRect, cursor) != 0;
    const RECT popupBounds = QuickSizePopupBounds(popupLayout, dpi);
    if (PtInRect(&popupBounds, cursor) == 0) {
        CancelQuickSizePopup();
        return false;
    }

    if (toolbarBrushSizeEditing_ && !clickedValue) {
        CancelToolbarBrushSizeInput();
    }
    if (toolbarZoomEditing_) {
        ApplyToolbarZoomInput();
        CancelToolbarZoomInput();
    }

    if (clickedSlider) {
        BeginToolbarBrushSliderDrag(cursor);
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    if (clickedValue) {
        BeginToolbarBrushSizeInput();
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    return true;
}

bool App::HandleToolbarRightClick(POINT cursor) {
    const UINT dpi = WindowDpi(window_);
    if (cursor.y >= VisibleToolbarHeight()) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const ToolbarLayout layout = BuildToolbarLayout(clientRect, palette_.size(), IsZoomed(window_), activeTool_, dpi);

    const auto showMenuForTool = [&](const RECT& rect, Tool tool) -> bool {
        if (PtInRect(&rect, cursor) == 0) {
            return false;
        }

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) {
            return true;
        }

        const bool alreadyInWheel = IsToolInWheel(tool);
        AppendMenuW(menu, MF_STRING, kCommandToolToggleQuickWheel, alreadyInWheel ? L"Remove from Quick Wheel" : L"Add to Quick Wheel");

        POINT anchor { cursor.x, cursor.y };
        ClientToScreen(window_, &anchor);
        const unsigned int selected = TrackPopupMenu(
            menu,
            TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
            anchor.x,
            anchor.y,
            0,
            window_,
            nullptr);
        DestroyMenu(menu);

        if (selected == kCommandToolToggleQuickWheel) {
            ToggleToolInWheel(tool);
        }
        return true;
    };

    return showMenuForTool(layout.selectToolRect, Tool::Select)
        || showMenuForTool(layout.textToolRect, Tool::Text)
        || showMenuForTool(layout.moveToolRect, Tool::Move)
        || showMenuForTool(layout.drawToolRect, Tool::Draw)
        || showMenuForTool(layout.panToolRect, Tool::Pan)
        || showMenuForTool(layout.eraseToolRect, Tool::Erase);
}

bool App::HandlePaletteClick(POINT cursor) {
    if (!IsToolbarVisible() && !wheel_.paletteWheelOpen) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const UINT dpi = WindowDpi(window_);
    const ToolbarLayout toolbarLayout = BuildToolbarLayout(clientRect, palette_.size(), IsZoomed(window_), activeTool_, dpi);

    if (PtInRect(&toolbarLayout.currentColorRect, cursor) != 0) {
        if (toolbarZoomEditing_) {
            ApplyToolbarZoomInput();
            CancelToolbarZoomInput();
        }
        CancelToolbarBrushSizeInput();
        wheel_.paletteMenuOpen = !wheel_.paletteMenuOpen;
        wheel_.paletteWheelOpen = false;
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    if (wheel_.paletteMenuOpen) {
        const PaletteMenuLayout menuLayout = BuildPaletteMenuLayout(toolbarLayout, palette_.size(), dpi);
        for (std::size_t index = 0; index < menuLayout.swatchRects.size(); ++index) {
            if (PtInRect(&menuLayout.swatchRects[index], cursor) != 0) {
                if (toolbarZoomEditing_) {
                    ApplyToolbarZoomInput();
                    CancelToolbarZoomInput();
                }
                CancelToolbarBrushSizeInput();
                SetColorIndex(index);
                return true;
            }
        }

        if (PtInRect(&menuLayout.panelRect, cursor) != 0) {
            if (toolbarZoomEditing_) {
                ApplyToolbarZoomInput();
                CancelToolbarZoomInput();
            }
            CancelToolbarBrushSizeInput();
            return true;
        }

        if (toolbarZoomEditing_) {
            ApplyToolbarZoomInput();
            CancelToolbarZoomInput();
        }
        CancelToolbarBrushSizeInput();
        ClosePaletteOverlays();
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    if (wheel_.paletteWheelOpen) {
        const std::size_t wheelEntryCount = palette_.size() + wheel_.tools.size();
        const PaletteWheelLayout wheelLayout = BuildPaletteWheelLayout(clientRect, wheel_.center, wheelEntryCount, VisibleToolbarHeight(), dpi);
        if (const auto index = PaletteWheelIndexAtPoint(wheelLayout, cursor, wheelEntryCount); index.has_value()) {
            if (*index < palette_.size()) {
                if (toolbarZoomEditing_) {
                    ApplyToolbarZoomInput();
                    CancelToolbarZoomInput();
                }
                CancelToolbarBrushSizeInput();
                SetColorIndex(*index);
            } else {
                if (toolbarZoomEditing_) {
                    ApplyToolbarZoomInput();
                    CancelToolbarZoomInput();
                }
                CancelToolbarBrushSizeInput();
                SetActiveTool(wheel_.tools[*index - palette_.size()]);
                ClosePaletteOverlays();
                InvalidateRect(window_, nullptr, FALSE);
            }
            return true;
        }

        if (toolbarZoomEditing_) {
            ApplyToolbarZoomInput();
            CancelToolbarZoomInput();
        }
        CancelToolbarBrushSizeInput();
        ClosePaletteOverlays();
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    return false;
}


COLORREF App::ToColorRef(std::uint32_t argb) const {
    const auto red = static_cast<unsigned char>((argb >> 16U) & 0xFFU);
    const auto green = static_cast<unsigned char>((argb >> 8U) & 0xFFU);
    const auto blue = static_cast<unsigned char>(argb & 0xFFU);
    return RGB(red, green, blue);
}

std::uint32_t App::ComposeColor(unsigned char red, unsigned char green, unsigned char blue) const {
    return 0xFF000000U | (static_cast<std::uint32_t>(red) << 16U) | (static_cast<std::uint32_t>(green) << 8U)
        | static_cast<std::uint32_t>(blue);
}

unsigned char App::GetColorChannel(std::uint32_t color, int shift) const {
    return static_cast<unsigned char>((color >> shift) & 0xFFU);
}


void App::PaintLiveTextEditOverlay(HDC hdc) const {
    if (window_ == nullptr || textEditWindow_ == nullptr || IsWindowVisible(textEditWindow_) == FALSE) {
        return;
    }

    RECT editRect {};
    if (GetWindowRect(textEditWindow_, &editRect) == FALSE) {
        return;
    }
    MapWindowPoints(HWND_DESKTOP, window_, reinterpret_cast<POINT*>(&editRect), 2);
    if (!HasRect(editRect)) {
        return;
    }

    const RECT frameRect = LiveTextEditFrameRect(editRect, WindowDpi(window_));
    const int savedDc = SaveDC(hdc);
    ExcludeClipRect(hdc, editRect.left, editRect.top, editRect.right, editRect.bottom);
    DrawDashedRect(hdc, frameRect, RGB(110, 204, 255), 2, 8, 5);
    RestoreDC(hdc, savedDc);
}

void App::PaintToolbar(HDC hdc, RECT clientRect) const {
    if (!IsToolbarVisible()) {
        return;
    }

    const int toolbarHeight = FullToolbarHeight();
    const float revealProgress = settings_.autoHideToolbar ? std::clamp(toolbarRevealProgress_, 0.0f, 1.0f) : 1.0f;
    const int toolbarOffsetY = static_cast<int>(std::lround(ToolbarOffsetYForProgress(toolbarHeight, revealProgress)));
    const int savedDc = SaveDC(hdc);
    if (savedDc != 0) {
        IntersectClipRect(hdc, clientRect.left, 0, clientRect.right, ToolbarVisibleClipHeight(toolbarHeight));
        if (toolbarOffsetY != 0) {
            SetViewportOrgEx(hdc, 0, toolbarOffsetY, nullptr);
        }
    }

    PaintToolbarContents(hdc, clientRect);

    if (savedDc != 0) {
        RestoreDC(hdc, savedDc);
    }
}

void App::PaintToolbarContents(HDC hdc, RECT clientRect) const {
    const UINT dpi = WindowDpi(window_);
    RECT toolbarRect = clientRect;
    const int toolbarHeight = FullToolbarHeight();
    toolbarRect.top = 0;
    toolbarRect.bottom = toolbarHeight;

    const COLORREF toolbarBackground = RGB(15, 18, 24);
    const COLORREF panelBorder = RGB(43, 52, 64);
    const COLORREF primaryText = RGB(236, 240, 245);
    const COLORREF accent = RGB(92, 137, 187);
    const COLORREF mutedText = RGB(156, 165, 178);

    HBRUSH toolbarBrush = CreateSolidBrush(toolbarBackground);
    FillRect(hdc, &toolbarRect, toolbarBrush);
    DeleteObject(toolbarBrush);

    SetBkMode(hdc, TRANSPARENT);

    HFONT bodyFont = CreateFontW(FontHeightForDpi(15, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT labelFont = CreateFontW(FontHeightForDpi(13, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    const ToolbarLayout layout = BuildToolbarLayout(clientRect, palette_.size(), IsZoomed(window_), activeTool_, dpi);
    HGDIOBJ previousFont = SelectObject(hdc, bodyFont);
    const ToolbarTitleLayout titleLayout = ResolveToolbarTitleLayout(layout, hdc, CurrentWindowTitle(), ZoomIndicatorLabel(), dpi);
    SelectObject(hdc, previousFont);

    previousFont = SelectObject(hdc, labelFont);
    SetTextColor(hdc, primaryText);

    const auto paintIconToolButton = [&](RECT rect, auto&& paintFallbackIcon, const wchar_t* svgFileName, bool selected, bool enabled = true) {
        const COLORREF iconColor = enabled
            ? (selected ? RGB(132, 191, 255) : primaryText)
            : RGB(102, 111, 124);
        if (const SvgIconData* icon = ToolbarSvgIcon(svgFileName); icon != nullptr) {
            PaintSvgIcon(hdc, rect, *icon, iconColor);
        } else {
            paintFallbackIcon(hdc, rect, iconColor);
        }
    };

    const auto paintWheelBadge = [&](RECT rect) {
        const int badgeInsetRight = ScaleForDpi(kQuickWheelBadgeInsetRightDip, dpi);
        const int badgeTop = rect.top + ScaleForDpi(kQuickWheelBadgeTopDip, dpi);
        const int badgeSize = std::max(1, ScaleForDpi(kQuickWheelBadgeSizeDip, dpi));
        RECT badgeRect = MakeRect(
            rect.right - badgeInsetRight - badgeSize,
            badgeTop,
            rect.right - badgeInsetRight,
            badgeTop + badgeSize);
        FillRoundedRect(hdc, badgeRect, RGB(244, 247, 252));
    };

    const auto paintSvgActionButton = [&](RECT rect, const wchar_t* svgFileName, const wchar_t* label, bool enabled) {
        const COLORREF iconColor = enabled ? primaryText : RGB(102, 111, 124);
        PaintSvgOrFallbackLabel(hdc, rect, svgFileName, label, iconColor, bodyFont);
    };

    const auto paintWindowButton = [&](RECT rect, int kind, bool hovered) {
        if (hovered) {
            FillSolidRect(hdc, rect, kind == 2 ? RGB(196, 43, 28) : RGB(60, 64, 72));
        }

        const COLORREF iconColor = RGB(248, 250, 252);
        HPEN pen = CreatePen(PS_SOLID, 1, iconColor);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

        const int centerX = (rect.left + rect.right) / 2;
        const int centerY = (rect.top + rect.bottom) / 2;
        const int lineHalfWidth = ScaleForDpi(8, dpi);
        const int boxHalfWidth = ScaleForDpi(6, dpi);
        const int boxHalfHeight = ScaleForDpi(5, dpi);
        const int closeOffset = ScaleForDpi(7, dpi);
        const int overlapOffset = ScaleForDpi(3, dpi);
        if (kind == 0) {
            const int lineY = centerY + ScaleForDpi(4, dpi);
            MoveToEx(hdc, centerX - lineHalfWidth, lineY, nullptr);
            LineTo(hdc, centerX + lineHalfWidth + 1, lineY);
        } else if (kind == 1) {
            if (IsZoomed(window_)) {
                Rectangle(hdc, centerX - boxHalfWidth, centerY - 1, centerX + boxHalfWidth + 1, centerY + boxHalfHeight + 2);
                MoveToEx(hdc, centerX - boxHalfWidth + overlapOffset, centerY - boxHalfHeight - overlapOffset, nullptr);
                LineTo(hdc, centerX + boxHalfWidth + overlapOffset + 1, centerY - boxHalfHeight - overlapOffset);
                LineTo(hdc, centerX + boxHalfWidth + overlapOffset + 1, centerY + boxHalfHeight - overlapOffset + 1);
                MoveToEx(hdc, centerX - boxHalfWidth + overlapOffset, centerY - boxHalfHeight - overlapOffset, nullptr);
                LineTo(hdc, centerX - boxHalfWidth + overlapOffset, centerY - boxHalfHeight + overlapOffset);
            } else {
                Rectangle(hdc, centerX - boxHalfWidth, centerY - boxHalfHeight, centerX + boxHalfWidth + 1, centerY + boxHalfHeight + 1);
            }
        } else {
            MoveToEx(hdc, centerX - closeOffset, centerY - closeOffset, nullptr);
            LineTo(hdc, centerX + closeOffset + 1, centerY + closeOffset + 1);
            MoveToEx(hdc, centerX + closeOffset, centerY - closeOffset, nullptr);
            LineTo(hdc, centerX - closeOffset - 1, centerY + closeOffset + 1);
        }

        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    };

    if (HasRect(layout.undoRect)) {
        paintSvgActionButton(layout.undoRect, L"undo.svg", L"Undo", CanUndo());
    }
    if (HasRect(layout.redoRect)) {
        paintSvgActionButton(layout.redoRect, L"redo.svg", L"Redo", CanRedo());
    }
    paintIconToolButton(layout.selectToolRect, PaintSelectToolIcon, L"lasso.svg", activeTool_ == Tool::Select);
    paintIconToolButton(layout.textToolRect, PaintTextToolIcon, L"text.svg", activeTool_ == Tool::Text);
    paintIconToolButton(layout.moveToolRect, PaintMoveToolIcon, L"cursor.svg", activeTool_ == Tool::Move || isMovingSelection_ || isTransformingImage_ || isTransformingStroke_);
    paintIconToolButton(layout.eraseToolRect, PaintEraseToolIcon, L"erase.svg", activeTool_ == Tool::Erase);
    paintIconToolButton(layout.drawToolRect, PaintDrawToolIcon, L"draw.svg", activeTool_ == Tool::Draw);
    paintIconToolButton(layout.panToolRect, PaintPanToolIcon, L"pan.svg", activeTool_ == Tool::Pan);
    if (IsToolInWheel(Tool::Select)) {
        paintWheelBadge(layout.selectToolRect);
    }
    if (IsToolInWheel(Tool::Move)) {
        paintWheelBadge(layout.moveToolRect);
    }
    if (IsToolInWheel(Tool::Text)) {
        paintWheelBadge(layout.textToolRect);
    }
    if (IsToolInWheel(Tool::Erase)) {
        paintWheelBadge(layout.eraseToolRect);
    }
    if (IsToolInWheel(Tool::Draw)) {
        paintWheelBadge(layout.drawToolRect);
    }
    if (IsToolInWheel(Tool::Pan)) {
        paintWheelBadge(layout.panToolRect);
    }

    paintWindowButton(layout.minimizeRect, 0, hoveredWindowButton_ == 0);
    paintWindowButton(layout.maximizeRect, 1, hoveredWindowButton_ == 1);
    paintWindowButton(layout.closeRect, 2, hoveredWindowButton_ == 2);

    if (HasRect(titleLayout.fileTitleRect)) {
        SetTextColor(hdc, (hoveredTitleMenu_ || titleMenuActive_) ? accent : mutedText);
        SelectObject(hdc, bodyFont);
        std::wstring title = CurrentWindowTitle();
        RECT titleTextRect = titleLayout.titleTextRect;
        DrawTextW(hdc, title.c_str(), -1, &titleTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (HasRect(titleLayout.fileTitleRect) && HasRect(titleLayout.zoomRect)) {
        HPEN dividerPen = CreatePen(PS_SOLID, 1, panelBorder);
        HGDIOBJ previousDividerPen = SelectObject(hdc, dividerPen);
        MoveToEx(hdc, titleLayout.dividerX, titleLayout.fileTitleRect.top + 11, nullptr);
        LineTo(hdc, titleLayout.dividerX, titleLayout.fileTitleRect.bottom - 11);
        SelectObject(hdc, previousDividerPen);
        DeleteObject(dividerPen);
    }

    if (HasRect(titleLayout.zoomRect)) {
        const std::wstring zoomLabel = ZoomIndicatorLabel();
        SetTextColor(hdc, toolbarZoomEditing_ ? RGB(244, 247, 252) : mutedText);
        SelectObject(hdc, bodyFont);
        if (toolbarZoomEditing_) {
            RECT zoomEntryRect = titleLayout.zoomTextRect;
            const int zoomTextWidth = MeasureToolbarTextWidth(hdc, zoomLabel);
            const int visualWidth = std::clamp(zoomTextWidth + 12, 34, static_cast<int>(titleLayout.zoomRect.right - titleLayout.zoomRect.left));
            RECT zoomVisualRect = titleLayout.zoomRect;
            zoomVisualRect.right = zoomVisualRect.left + visualWidth;
            zoomVisualRect.top += 7;
            zoomVisualRect.bottom -= 7;
            StrokeRoundedRect(hdc, zoomVisualRect, accent, 1);
            zoomEntryRect = zoomVisualRect;
            zoomEntryRect.left += 5;
            zoomEntryRect.right -= 5;
            DrawTextW(hdc, zoomLabel.c_str(), -1, &zoomEntryRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            SIZE textSize {};
            if (GetTextExtentPoint32W(hdc, zoomLabel.c_str(), static_cast<int>(zoomLabel.size()), &textSize) != 0) {
                const int caretX = std::min(static_cast<int>(zoomEntryRect.right) - 1, static_cast<int>(zoomEntryRect.left) + static_cast<int>(textSize.cx) + 1);
                PaintVerticalCaret(hdc, caretX, zoomVisualRect.top + 4, zoomVisualRect.bottom - 4, RGB(244, 247, 252));
            }
        } else {
            RECT zoomTextRect = titleLayout.zoomTextRect;
            DrawTextW(hdc, zoomLabel.c_str(), -1, &zoomTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    FillRoundedRect(hdc, layout.currentColorRect, ToColorRef(palette_[colorIndex_]));
    StrokeRoundedRect(hdc, layout.currentColorRect, wheel_.paletteMenuOpen ? RGB(244, 247, 252) : panelBorder, wheel_.paletteMenuOpen ? 2 : 1);

    SelectObject(hdc, previousFont);
    DeleteObject(bodyFont);
    DeleteObject(labelFont);
}

void App::PaintPaletteMenu(HDC hdc, RECT clientRect) const {
    if (!wheel_.paletteMenuOpen) {
        return;
    }

    const UINT dpi = WindowDpi(window_);
    const ToolbarLayout toolbarLayout = BuildToolbarLayout(clientRect, palette_.size(), IsZoomed(window_), activeTool_, dpi);
    const PaletteMenuLayout layout = BuildPaletteMenuLayout(toolbarLayout, palette_.size(), dpi);
    PaintRoundedPanel(hdc, layout.panelRect, ui::kPanelFill, ui::kPanelBorder);

    for (std::size_t index = 0; index < layout.swatchRects.size(); ++index) {
        const RECT swatchRect = layout.swatchRects[index];
        FillRoundedRect(hdc, swatchRect, ToColorRef(palette_[index]));
        StrokeRoundedRect(hdc, swatchRect, index == colorIndex_ ? ui::kSelectionBorder : ui::kPanelBorder, index == colorIndex_ ? 2 : 1);
    }
}

void App::PaintPaletteWheel(HDC hdc, RECT clientRect) const {
    if (!wheel_.paletteWheelOpen) {
        return;
    }

    const std::size_t wheelEntryCount = palette_.size() + wheel_.tools.size();
    const UINT dpi = WindowDpi(window_);
    const PaletteWheelLayout layout = BuildPaletteWheelLayout(clientRect, wheel_.center, wheelEntryCount, VisibleToolbarHeight(), dpi);
    if (wheelEntryCount == 0U) {
        return;
    }

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const int outerDiameter = layout.outerRadius * 2;
    const int innerDiameter = layout.innerRadius * 2;
    Gdiplus::RectF outerRect(
        static_cast<Gdiplus::REAL>(layout.center.x - layout.outerRadius),
        static_cast<Gdiplus::REAL>(layout.center.y - layout.outerRadius),
        static_cast<Gdiplus::REAL>(outerDiameter),
        static_cast<Gdiplus::REAL>(outerDiameter));
    const int shadowInset = PaletteWheelShadowInset(dpi);
    const int iconHalfSize = PaletteWheelIconHalfSize(dpi);
    const int hoverOffset = PaletteWheelHoverOffset(dpi);
    const int hoverGrow = PaletteWheelHoverGrow(dpi);

    Gdiplus::SolidBrush haloBrush(Gdiplus::Color(210, 18, 24, 32));
    graphics.FillEllipse(
        &haloBrush,
        static_cast<Gdiplus::REAL>(layout.center.x - (layout.outerRadius + shadowInset)),
        static_cast<Gdiplus::REAL>(layout.center.y - (layout.outerRadius + shadowInset)),
        static_cast<Gdiplus::REAL>((layout.outerRadius + shadowInset) * 2),
        static_cast<Gdiplus::REAL>((layout.outerRadius + shadowInset) * 2));

    const auto hoverProgress = [&](std::size_t index) {
        if (index >= paletteWheelHoverProgresses_.size()) {
            return 0.0f;
        }
        return std::clamp(paletteWheelHoverProgresses_[index], 0.0f, 1.0f);
    };

    const auto isAnimatingEntry = [&](std::size_t index) {
        return hoverProgress(index) > 0.001f;
    };

    const float sweep = 360.0f / static_cast<float>(wheelEntryCount);
    const auto paintSegment = [&](std::size_t index, bool paintFill, bool paintStroke, bool paintIcon) {
        const float baseStart = -90.0f + (sweep * static_cast<float>(index));
        const float centerAngleDegrees = baseStart + (sweep * 0.5f);
        const float centerAngleRadians = centerAngleDegrees * (3.14159265358979323846f / 180.0f);
        const float progress = hoverProgress(index);
        const int iconOffsetX = static_cast<int>(std::lround(std::cos(centerAngleRadians) * static_cast<float>(hoverOffset) * progress));
        const int iconOffsetY = static_cast<int>(std::lround(std::sin(centerAngleRadians) * static_cast<float>(hoverOffset) * progress));
        const int grow = static_cast<int>(std::lround(static_cast<float>(hoverGrow) * progress));
        Gdiplus::RectF segmentOuterRect(
            outerRect.X - static_cast<Gdiplus::REAL>(grow),
            outerRect.Y - static_cast<Gdiplus::REAL>(grow),
            outerRect.Width + static_cast<Gdiplus::REAL>(grow * 2),
            outerRect.Height + static_cast<Gdiplus::REAL>(grow * 2));
        Gdiplus::RectF segmentInnerRect(
            static_cast<Gdiplus::REAL>(layout.center.x - layout.innerRadius),
            static_cast<Gdiplus::REAL>(layout.center.y - layout.innerRadius),
            static_cast<Gdiplus::REAL>(innerDiameter),
            static_cast<Gdiplus::REAL>(innerDiameter));
        const auto pointOnArc = [](const Gdiplus::RectF& rect, float angleDegrees) {
            const float radians = angleDegrees * (3.14159265358979323846f / 180.0f);
            return Gdiplus::PointF(
                rect.X + (rect.Width * 0.5f) + (std::cos(radians) * rect.Width * 0.5f),
                rect.Y + (rect.Height * 0.5f) + (std::sin(radians) * rect.Height * 0.5f));
        };
        const auto buildSegmentPath = [&](Gdiplus::GraphicsPath& path, float startAngle, float sweepAngle) {
            const Gdiplus::PointF arcStart = pointOnArc(segmentOuterRect, startAngle);
            const Gdiplus::PointF arcEnd = pointOnArc(segmentOuterRect, startAngle + sweepAngle);
            const Gdiplus::PointF innerEnd = pointOnArc(segmentInnerRect, startAngle + sweepAngle);
            const Gdiplus::PointF innerStart = pointOnArc(segmentInnerRect, startAngle);
            path.StartFigure();
            path.AddArc(segmentOuterRect, startAngle, sweepAngle);
            path.AddLine(arcEnd, innerEnd);
            path.AddArc(segmentInnerRect, startAngle + sweepAngle, -sweepAngle);
            path.AddLine(innerStart, arcStart);
            path.CloseFigure();
        };

        const bool isColorEntry = index < palette_.size();
        const bool selected = isColorEntry ? index == colorIndex_ : activeTool_ == wheel_.tools[index - palette_.size()];
        const Gdiplus::Color segmentColor = isColorEntry
            ? Gdiplus::Color(255, GetRValue(ToColorRef(palette_[index])), GetGValue(ToColorRef(palette_[index])), GetBValue(ToColorRef(palette_[index])))
            : (selected ? Gdiplus::Color(255, 61, 118, 164) : Gdiplus::Color(245, 32, 41, 52));
        Gdiplus::GraphicsPath fillPath;
        Gdiplus::GraphicsPath strokePath;
        if (wheelEntryCount == 1U) {
            fillPath.AddEllipse(segmentOuterRect);
            strokePath.AddEllipse(segmentOuterRect);
        } else {
            buildSegmentPath(fillPath, baseStart, sweep);
            buildSegmentPath(strokePath, baseStart, sweep);
        }

        if (paintFill) {
            Gdiplus::SolidBrush brush(segmentColor);
            graphics.FillPath(&brush, &fillPath);
        }

        if (paintStroke) {
            const float strokeWidth = PaletteWheelStrokeWidth(selected ? kPaletteWheelSelectedStrokeDip : kPaletteWheelStrokeDip, dpi);
            Gdiplus::Pen separatorPen(
                Gdiplus::Color(selected ? 255 : 120, 244, 247, 252),
                strokeWidth);
            graphics.DrawPath(&separatorPen, &strokePath);
        }

        if (paintIcon && !isColorEntry) {
            const float iconRadius = static_cast<float>(layout.innerRadius + ((layout.outerRadius - layout.innerRadius) * 0.5f))
                + (static_cast<float>(grow) * 0.5f);
            const int iconCenterX = layout.center.x + iconOffsetX + static_cast<int>(std::lround(std::cos(centerAngleRadians) * iconRadius));
            const int iconCenterY = layout.center.y + iconOffsetY + static_cast<int>(std::lround(std::sin(centerAngleRadians) * iconRadius));
            const RECT iconRect = MakeRect(
                iconCenterX - iconHalfSize,
                iconCenterY - iconHalfSize,
                iconCenterX + iconHalfSize,
                iconCenterY + iconHalfSize);
            const COLORREF iconColor = selected ? RGB(255, 255, 255) : RGB(232, 238, 245);
            if (const SvgIconData* icon = ToolbarSvgIcon(ToolSvgFileName(wheel_.tools[index - palette_.size()])); icon != nullptr) {
                PaintSvgIcon(hdc, iconRect, *icon, iconColor);
            }
        }
    };

    const auto isSelectedEntry = [&](std::size_t index) {
        return index < palette_.size()
            ? index == colorIndex_
            : activeTool_ == wheel_.tools[index - palette_.size()];
    };

    const auto pointOnWheelRadius = [&](float angleDegrees, int radius) {
        const float radians = angleDegrees * (3.14159265358979323846f / 180.0f);
        return Gdiplus::PointF(
            static_cast<Gdiplus::REAL>(layout.center.x) + (std::cos(radians) * static_cast<Gdiplus::REAL>(radius)),
            static_cast<Gdiplus::REAL>(layout.center.y) + (std::sin(radians) * static_cast<Gdiplus::REAL>(radius)));
    };

    const auto paintBaseWheelStroke = [&]() {
        Gdiplus::Pen separatorPen(
            Gdiplus::Color(120, 244, 247, 252),
            PaletteWheelStrokeWidth(kPaletteWheelStrokeDip, dpi));

        if (wheelEntryCount <= 1U) {
            graphics.DrawEllipse(&separatorPen, outerRect);
            return;
        }

        for (std::size_t index = 0; index < wheelEntryCount; ++index) {
            const std::size_t previousIndex = index == 0U ? wheelEntryCount - 1U : index - 1U;
            if (!isAnimatingEntry(index)) {
                graphics.DrawArc(
                    &separatorPen,
                    outerRect,
                    -90.0f + (sweep * static_cast<float>(index)),
                    sweep);
            }

            if (!isAnimatingEntry(index) && !isAnimatingEntry(previousIndex)) {
                const float dividerAngle = -90.0f + (sweep * static_cast<float>(index));
                const Gdiplus::PointF innerPoint = pointOnWheelRadius(dividerAngle, layout.innerRadius);
                const Gdiplus::PointF outerPoint = pointOnWheelRadius(dividerAngle, layout.outerRadius);
                graphics.DrawLine(&separatorPen, innerPoint, outerPoint);
            }
        }
    };

    for (std::size_t index = 0; index < wheelEntryCount; ++index) {
        if (!isAnimatingEntry(index)) {
            paintSegment(index, true, false, false);
        }
    }
    for (std::size_t index = 0; index < wheelEntryCount; ++index) {
        if (isAnimatingEntry(index)) {
            paintSegment(index, true, false, false);
        }
    }

    paintBaseWheelStroke();

    for (std::size_t index = 0; index < wheelEntryCount; ++index) {
        if (isAnimatingEntry(index) && !isSelectedEntry(index)) {
            paintSegment(index, false, true, false);
        }
    }
    for (std::size_t index = 0; index < wheelEntryCount; ++index) {
        if (isSelectedEntry(index) && !isAnimatingEntry(index)) {
            paintSegment(index, false, true, false);
        }
    }
    for (std::size_t index = 0; index < wheelEntryCount; ++index) {
        if (isSelectedEntry(index) && isAnimatingEntry(index)) {
            paintSegment(index, false, true, false);
        }
    }
    for (std::size_t index = 0; index < wheelEntryCount; ++index) {
        if (!isAnimatingEntry(index)) {
            paintSegment(index, false, false, true);
        }
    }
    for (std::size_t index = 0; index < wheelEntryCount; ++index) {
        if (isAnimatingEntry(index)) {
            paintSegment(index, false, false, true);
        }
    }

    Gdiplus::SolidBrush cutoutBrush(Gdiplus::Color(255, 18, 24, 32));
    graphics.FillEllipse(
        &cutoutBrush,
        static_cast<Gdiplus::REAL>(layout.center.x - layout.innerRadius),
        static_cast<Gdiplus::REAL>(layout.center.y - layout.innerRadius),
        static_cast<Gdiplus::REAL>(innerDiameter),
        static_cast<Gdiplus::REAL>(innerDiameter));

    Gdiplus::Pen centerBorder(Gdiplus::Color(180, 244, 247, 252), PaletteWheelStrokeWidth(kPaletteWheelBorderDip, dpi));
    graphics.DrawEllipse(
        &centerBorder,
        static_cast<Gdiplus::REAL>(layout.center.x - layout.innerRadius),
        static_cast<Gdiplus::REAL>(layout.center.y - layout.innerRadius),
        static_cast<Gdiplus::REAL>(innerDiameter),
        static_cast<Gdiplus::REAL>(innerDiameter));
}

void App::PaintPdfSidebar(HDC hdc, RECT clientRect) {
    if (!IsPdfSidebarVisible()) {
        return;
    }

    const RECT sidebarRect = PdfSidebarRect(clientRect);
    SaveDC(hdc);
    IntersectClipRect(hdc, sidebarRect.left, sidebarRect.top, sidebarRect.right, sidebarRect.bottom);
    PaintPdfSidebarContents(hdc, sidebarRect);
    RestoreDC(hdc, -1);
}

void App::PaintPdfSidebarContents(HDC hdc, RECT sidebarRect) {
    HBRUSH sidebarBrush = CreateSolidBrush(RGB(18, 22, 29));
    FillRect(hdc, &sidebarRect, sidebarBrush);
    DeleteObject(sidebarBrush);

    HPEN dividerPen = CreatePen(PS_SOLID, 1, RGB(43, 52, 64));
    HGDIOBJ oldPen = SelectObject(hdc, dividerPen);
    MoveToEx(hdc, sidebarRect.right - 1, sidebarRect.top, nullptr);
    LineTo(hdc, sidebarRect.right - 1, sidebarRect.bottom);
    SelectObject(hdc, oldPen);
    DeleteObject(dividerPen);

    SetBkMode(hdc, TRANSPARENT);
    HFONT labelFont = CreateFontW(FontHeightForDpi(14, WindowDpi(window_)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HGDIOBJ previousFont = SelectObject(hdc, labelFont);

    const COLORREF panelBorder = RGB(43, 52, 64);
    const COLORREF primaryText = RGB(236, 240, 245);
    const COLORREF mutedText = RGB(156, 165, 178);
    const COLORREF accent = RGB(92, 137, 187);
    const UINT dpi = WindowDpi(window_);
    const SidebarHeaderLayout headerLayout = BuildSidebarHeaderLayout(sidebarRect, dpi, IsZoomed(window_) != FALSE);
    const SidebarMetrics metrics = BuildSidebarMetrics(dpi);

    RECT bottomBarRect = MakeRect(sidebarRect.left, headerLayout.contentRect.bottom, sidebarRect.right, sidebarRect.bottom);
    FillSolidRect(hdc, bottomBarRect, RGB(15, 18, 24));
    HPEN bottomBarDividerPen = CreatePen(PS_SOLID, 1, panelBorder);
    HGDIOBJ previousBottomBarPen = SelectObject(hdc, bottomBarDividerPen);
    MoveToEx(hdc, bottomBarRect.left, bottomBarRect.top, nullptr);
    LineTo(hdc, bottomBarRect.right, bottomBarRect.top);
    SelectObject(hdc, previousBottomBarPen);
    DeleteObject(bottomBarDividerPen);

    const auto paintFlatActionButton = [&](RECT rect, const wchar_t* svgFileName, const wchar_t* fallbackLabel, bool enabled, bool accentContent) {
        const COLORREF contentColor = enabled
            ? (accentContent ? accent : primaryText)
            : RGB(102, 111, 124);
        const int iconSize = ScaleForDpi(26, dpi);
        const int availableWidth = std::max(1, static_cast<int>(rect.right - rect.left));
        const int contentLeft = rect.left + std::max(0, (availableWidth - iconSize) / 2);
        RECT iconRect = MakeRect(
            contentLeft,
            rect.top + ((rect.bottom - rect.top - iconSize) / 2),
            contentLeft + iconSize,
            rect.top + ((rect.bottom - rect.top - iconSize) / 2) + iconSize);
        PaintSvgOrFallbackLabel(hdc, iconRect, svgFileName, fallbackLabel, contentColor, labelFont);
    };
    paintFlatActionButton(headerLayout.addPageRect, L"new-page.svg", L"+", true, true);
    paintFlatActionButton(headerLayout.newPageSettingsRect, L"settings.svg", L"...", true, false);

    const std::wstring pageLabel = PageIndicatorLabel();
    RECT pageLabelRect = headerLayout.pageLabelRect;
    if (pageJumpEditing_) {
        RECT entryVisualRect = pageLabelRect;
        entryVisualRect.top += ScaleForDpi(5, dpi);
        entryVisualRect.bottom -= ScaleForDpi(5, dpi);
        StrokeRoundedRect(hdc, entryVisualRect, accent, 1);
        RECT entryTextRect = entryVisualRect;
        entryTextRect.left += ScaleForDpi(6, dpi);
        entryTextRect.right -= ScaleForDpi(6, dpi);
        SetTextColor(hdc, RGB(244, 247, 252));
        DrawTextW(hdc, pageLabel.c_str(), -1, &entryTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SIZE textSize {};
        if (GetTextExtentPoint32W(hdc, pageLabel.c_str(), static_cast<int>(pageLabel.size()), &textSize) != 0) {
            const int caretX = std::min(entryTextRect.right - 1, entryTextRect.left + textSize.cx + 1);
            HPEN caretPen = CreatePen(PS_SOLID, 1, RGB(244, 247, 252));
            HGDIOBJ oldCaretPen = SelectObject(hdc, caretPen);
            MoveToEx(hdc, caretX, entryVisualRect.top + ScaleForDpi(4, dpi), nullptr);
            LineTo(hdc, caretX, entryVisualRect.bottom - ScaleForDpi(4, dpi));
            SelectObject(hdc, oldCaretPen);
            DeleteObject(caretPen);
        }
    } else {
        SetTextColor(hdc, mutedText);
        RECT labelInsetRect = pageLabelRect;
        labelInsetRect.left += ScaleForDpi(2, dpi);
        labelInsetRect.right -= ScaleForDpi(2, dpi);
        DrawTextW(hdc, pageLabel.c_str(), -1, &labelInsetRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    SaveDC(hdc);
    IntersectClipRect(
        hdc,
        headerLayout.contentRect.left,
        headerLayout.contentRect.top,
        headerLayout.contentRect.right,
        headerLayout.contentRect.bottom);

    const int itemWidth = std::max(1, static_cast<int>(headerLayout.headerRect.right - headerLayout.headerRect.left) - (metrics.itemInset * 2));
    const int left = ((sidebarRect.left + sidebarRect.right) - itemWidth) / 2;
    int top = headerLayout.contentRect.top + metrics.contentPadding - pdfSidebarScrollY_;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        const Page& page = document_.PageAt(pageIndex);
        const float aspect = page.width > 0.0f ? (page.height / page.width) : (11.0f / 8.5f);
        const int thumbHeight = std::max(metrics.minThumbHeight, static_cast<int>(std::lround(static_cast<float>(itemWidth) * aspect)));
        RECT thumbRect = MakeRect(left, top, left + itemWidth, top + thumbHeight);
        RECT itemRect = MakeRect(thumbRect.left, thumbRect.top, thumbRect.right, thumbRect.bottom + metrics.labelHeight);
        top = itemRect.bottom + metrics.itemGap;

        if (itemRect.bottom < headerLayout.contentRect.top || itemRect.top > headerLayout.contentRect.bottom) {
            continue;
        }

        const bool selected = pageIndex == document_.CurrentPageIndex();
        if (selected) {
            HBRUSH selectedBrush = CreateSolidBrush(RGB(31, 41, 55));
            FillRect(hdc, &itemRect, selectedBrush);
            DeleteObject(selectedBrush);
        }

        HBRUSH pageBrush = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &thumbRect, pageBrush);
        DeleteObject(pageBrush);

        EnsureSidebarThumbnailCache(pageIndex, thumbRect.right - thumbRect.left, thumbRect.bottom - thumbRect.top);
        const SidebarThumbnailCache* cache = pageIndex < sidebarThumbnailCaches_.size() ? &sidebarThumbnailCaches_[pageIndex] : nullptr;
        if (cache != nullptr && cache->valid && !cache->pixels.empty()) {
            BITMAPINFO bitmapInfo {};
            bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
            bitmapInfo.bmiHeader.biWidth = cache->width;
            bitmapInfo.bmiHeader.biHeight = -cache->height;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;

            SetDIBitsToDevice(
                hdc,
                thumbRect.left,
                thumbRect.top,
                static_cast<DWORD>(cache->width),
                static_cast<DWORD>(cache->height),
                0,
                0,
                0,
                static_cast<UINT>(cache->height),
                cache->pixels.data(),
                &bitmapInfo,
                DIB_RGB_COLORS);
        } else {
            HPEN placeholderPen = CreatePen(PS_SOLID, 1, RGB(205, 211, 220));
            HGDIOBJ oldPlaceholderPen = SelectObject(hdc, placeholderPen);
            MoveToEx(hdc, thumbRect.left + metrics.placeholderInset, thumbRect.top + metrics.placeholderInset, nullptr);
            LineTo(hdc, thumbRect.right - metrics.placeholderInset, thumbRect.top + metrics.placeholderInset);
            MoveToEx(hdc, thumbRect.left + metrics.placeholderInset, thumbRect.top + metrics.placeholderInset + metrics.placeholderLineGap, nullptr);
            LineTo(hdc, thumbRect.right - (metrics.placeholderInset + ScaleForDpi(8, dpi)), thumbRect.top + metrics.placeholderInset + metrics.placeholderLineGap);
            MoveToEx(hdc, thumbRect.left + metrics.placeholderInset, thumbRect.top + metrics.placeholderInset + (metrics.placeholderLineGap * 2), nullptr);
            LineTo(hdc, thumbRect.right - (metrics.placeholderInset + ScaleForDpi(16, dpi)), thumbRect.top + metrics.placeholderInset + (metrics.placeholderLineGap * 2));
            SelectObject(hdc, oldPlaceholderPen);
            DeleteObject(placeholderPen);
        }

        HPEN borderPen = CreatePen(PS_SOLID, selected ? 2 : 1, selected ? RGB(92, 137, 187) : RGB(71, 82, 96));
        HGDIOBJ itemOldPen = SelectObject(hdc, borderPen);
        HGDIOBJ itemOldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, thumbRect.left, thumbRect.top, thumbRect.right, thumbRect.bottom);
        SelectObject(hdc, itemOldBrush);
        SelectObject(hdc, itemOldPen);
        DeleteObject(borderPen);

        SetTextColor(hdc, selected ? RGB(244, 247, 252) : RGB(187, 195, 206));
        RECT labelRect = MakeRect(itemRect.left, thumbRect.bottom + ScaleForDpi(2, dpi), itemRect.right, itemRect.bottom);
        const std::wstring label = std::to_wstring(pageIndex + 1U);
        DrawTextW(hdc, label.c_str(), -1, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RestoreDC(hdc, -1);
    SelectObject(hdc, previousFont);
    DeleteObject(labelFont);
}

void App::PaintSelectionOverlay(HDC hdc, RECT canvasRect) const {
    if (!isSelecting_) {
        return;
    }

    SaveDC(hdc);
    IntersectClipRect(hdc, canvasRect.left, canvasRect.top, canvasRect.right, canvasRect.bottom);
    if (selectionBoxOverride_ || selectionMode_ == SelectionMode::Box) {
        const RECT selectionRect = MakeRect(
            std::min(selectionAnchorCursor_.x, selectionCurrentCursor_.x),
            std::min(selectionAnchorCursor_.y, selectionCurrentCursor_.y),
            std::max(selectionAnchorCursor_.x, selectionCurrentCursor_.x),
            std::max(selectionAnchorCursor_.y, selectionCurrentCursor_.y));
        DrawRectangleAA(hdc, selectionRect, RGB(124, 211, 255), 1.5f, Gdiplus::DashStyleDash);
    } else {
        const std::vector<Vec2> selectionPolygon = CurrentSelectionLassoPolygon();
        std::vector<POINT> points;
        points.reserve(selectionPolygon.size());
        for (const Vec2& point : selectionPolygon) {
            points.push_back({
                static_cast<LONG>(std::lround(point.x)),
                static_cast<LONG>(std::lround(point.y)),
            });
        }
        if (points.size() >= 2U) {
            DrawPolylineAA(hdc, points.data(), static_cast<int>(points.size()), RGB(124, 211, 255), 1.5f, Gdiplus::DashStyleDash);
            if (points.size() >= 3U) {
                POINT closingLine[2] { points.back(), points.front() };
                DrawPolylineAA(hdc, closingLine, 2, RGB(124, 211, 255), 1.5f, Gdiplus::DashStyleDash);
            }
        }
    }
    RestoreDC(hdc, -1);
}

void App::PaintInteractionPreview(HDC hdc, const PageView& pageView, bool showSelectToolSelectionAffordances) const {
    if (interactionPreviewMode_ == InteractionPreviewMode::None || pageView.pageIndex >= document_.PageCount()) {
        return;
    }
    const UINT dpi = window_ != nullptr ? WindowDpi(window_) : SystemDpi();

    if (pageView.pageIndex < interactionPreviewImages_.size()) {
        Gdiplus::Graphics imageGraphics(hdc);
        imageGraphics.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
        imageGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);
        imageGraphics.SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
        for (const InteractionPreviewImage& entry : interactionPreviewImages_[pageView.pageIndex]) {
            const PageImage& image = entry.image;
            if (image.pixels.empty() || image.pixelWidth <= 0 || image.pixelHeight <= 0) {
                continue;
            }

            Gdiplus::Bitmap bitmap(
                image.pixelWidth,
                image.pixelHeight,
                image.stride,
                PixelFormat32bppRGB,
                const_cast<BYTE*>(reinterpret_cast<const BYTE*>(image.pixels.data())));
            const Gdiplus::REAL centerX = static_cast<Gdiplus::REAL>((image.x + (image.width * 0.5f)) * pageView.scale + pageView.rect.left);
            const Gdiplus::REAL centerY = static_cast<Gdiplus::REAL>((image.y + (image.height * 0.5f)) * pageView.scale + pageView.rect.top);
            const Gdiplus::REAL drawWidth = static_cast<Gdiplus::REAL>(image.width * pageView.scale);
            const Gdiplus::REAL drawHeight = static_cast<Gdiplus::REAL>(image.height * pageView.scale);

            imageGraphics.ResetTransform();
            imageGraphics.TranslateTransform(centerX, centerY);
            imageGraphics.RotateTransform(image.rotationDegrees);
            imageGraphics.DrawImage(&bitmap, -drawWidth * 0.5f, -drawHeight * 0.5f, drawWidth, drawHeight);

            const auto corners = ImageCorners(image);
            POINT polygon[5] {};
            for (std::size_t index = 0; index < corners.size(); ++index) {
                const Vec2 screenPoint = ScreenPointFromPagePoint(corners[index], pageView.rect, pageView.scale);
                polygon[index].x = static_cast<LONG>(std::lround(screenPoint.x));
                polygon[index].y = static_cast<LONG>(std::lround(screenPoint.y));
            }
            polygon[4] = polygon[0];
            DrawPolylineAA(hdc, polygon, 5, RGB(110, 204, 255), 2.0f, Gdiplus::DashStyleDash);

            if ((interactionPreviewMode_ == InteractionPreviewMode::ImageTransform || interactionPreviewMode_ == InteractionPreviewMode::MoveSelection)
                && (activeTool_ == Tool::Move || isMovingSelection_ || isTransformingImage_
                    || (activeTool_ == Tool::Select && showSelectToolSelectionAffordances))) {
                for (std::size_t index = 0; index < corners.size(); ++index) {
                    const Vec2 screenPoint = ScreenPointFromPagePoint(corners[index], pageView.rect, pageView.scale);
                    PaintSelectionHandle(hdc, static_cast<int>(std::lround(screenPoint.x)), static_cast<int>(std::lround(screenPoint.y)), dpi);
                }
            }
        }
        imageGraphics.ResetTransform();
    }

    if (pageView.pageIndex < interactionPreviewStrokes_.size()) {
        for (const InteractionPreviewStroke& entry : interactionPreviewStrokes_[pageView.pageIndex]) {
            PaintStroke(hdc, entry.stroke, pageView, false, true);
        }

        const bool shouldDrawHandles = (interactionPreviewMode_ == InteractionPreviewMode::StrokeTransform || interactionPreviewMode_ == InteractionPreviewMode::MoveSelection)
            && (activeTool_ == Tool::Move || isMovingSelection_ || isTransformingStroke_
                || (activeTool_ == Tool::Select && showSelectToolSelectionAffordances));
        if (shouldDrawHandles && !interactionPreviewStrokes_[pageView.pageIndex].empty()) {
            float minX = document_.PageAt(pageView.pageIndex).width;
            float minY = document_.PageAt(pageView.pageIndex).height;
            float maxX = 0.0f;
            float maxY = 0.0f;
            for (const InteractionPreviewStroke& entry : interactionPreviewStrokes_[pageView.pageIndex]) {
                for (const Vec2& point : entry.stroke.points) {
                    minX = std::min(minX, point.x);
                    minY = std::min(minY, point.y);
                    maxX = std::max(maxX, point.x);
                    maxY = std::max(maxY, point.y);
                }
            }

            const int left = pageView.rect.left + static_cast<int>(std::lround(minX * pageView.scale));
            const int top = pageView.rect.top + static_cast<int>(std::lround(minY * pageView.scale));
            const int right = pageView.rect.left + static_cast<int>(std::lround(maxX * pageView.scale));
            const int bottom = pageView.rect.top + static_cast<int>(std::lround(maxY * pageView.scale));
            DrawRectangleAA(hdc, MakeRect(left, top, right, bottom), RGB(110, 204, 255), 2.0f, Gdiplus::DashStyleDash);
        }
    }

    if (pageView.pageIndex < interactionPreviewTexts_.size()) {
        for (const InteractionPreviewText& entry : interactionPreviewTexts_[pageView.pageIndex]) {
            PaintText(hdc, entry.text, pageView);
            if (const auto textRect = PageTextBoundsRect(entry.text, document_.PageAt(pageView.pageIndex).width, pageView.scale, 4); textRect.has_value()) {
                RECT screenRect = *textRect;
                OffsetRect(&screenRect, pageView.rect.left, pageView.rect.top);
                DrawRectangleAA(hdc, screenRect, RGB(110, 204, 255), 2.0f, Gdiplus::DashStyleDash);
                const bool shouldDrawHandles = (interactionPreviewMode_ == InteractionPreviewMode::TextTransform || interactionPreviewMode_ == InteractionPreviewMode::MoveSelection)
                    && (activeTool_ == Tool::Move || isMovingSelection_ || isTransformingText_
                        || (activeTool_ == Tool::Select && showSelectToolSelectionAffordances));
                if (shouldDrawHandles) {
                    const int left = screenRect.left;
                    const int top = screenRect.top;
                    const int right = screenRect.right;
                    const int bottom = screenRect.bottom;
                    const int centerX = (left + right) / 2;
                    const int centerY = (top + bottom) / 2;
                    PaintSelectionHandle(hdc, left, top, dpi);
                    PaintSelectionHandle(hdc, centerX, top, dpi);
                    PaintSelectionHandle(hdc, right, top, dpi);
                    PaintSelectionHandle(hdc, right, centerY, dpi);
                    PaintSelectionHandle(hdc, right, bottom, dpi);
                    PaintSelectionHandle(hdc, centerX, bottom, dpi);
                    PaintSelectionHandle(hdc, left, bottom, dpi);
                    PaintSelectionHandle(hdc, left, centerY, dpi);
                }
            }
        }
    }
}

float App::EraserRadiusPixels() const {
    return std::max(kEraserMinRadiusPixels, settings_.eraseBrushSize * 4.0f);
}

float App::EraserDiameterPixels() const {
    return EraserRadiusPixels() * 2.0f;
}

float App::QuickSizePreviewDiameterForTool(Tool tool, RECT clientRect) const {
    const float brushSize = BrushSizeForTool(tool);
    if (tool == Tool::Erase) {
        return EraserDiameterPixels();
    }

    float pageScale = std::max(0.1f, zoom_);
    if (document_.PageCount() > 0U) {
        const std::size_t pageIndex = std::min(document_.CurrentPageIndex(), document_.PageCount() - 1U);
        const PageView pageView = CalculatePageView(clientRect, pageIndex);
        if (pageView.valid && pageView.scale > 0.0f) {
            pageScale = pageView.scale;
        }
    }

    return std::max(1.25f, brushSize * pageScale / 2.15f);
}

int App::QuickSizePreviewExtentForTool(Tool tool, RECT clientRect) const {
    const UINT dpi = WindowDpi(window_);
    return std::max(
        std::max(1, ScaleForDpi(kQuickSizePreviewMinDip, dpi)),
        static_cast<int>(std::ceil(QuickSizePreviewDiameterForTool(tool, clientRect))) + ScaleForDpi(2, dpi));
}

HCURSOR App::EraserCursorForCurrentSize() {
    const int diameter = std::max(1, static_cast<int>(std::lround(EraserDiameterPixels())));
    if (eraserCursor_ != nullptr && eraserCursorDiameter_ == diameter) {
        return eraserCursor_;
    }

    ReleaseEraserCursor();
    eraserCursor_ = CreateSquareBrushCursor(diameter, RGB(255, 255, 255));
    eraserCursorDiameter_ = eraserCursor_ != nullptr ? diameter : 0;
    return eraserCursor_;
}

HCURSOR App::DrawCursorForCurrentSize(RECT clientRect) {
    static_cast<void>(clientRect);
    if (settings_.drawCursorKind == DrawCursorKind::PrecisionCrosshair) {
        ReleaseDrawCursor();
        return LoadCursorW(nullptr, IDC_CROSS);
    }

    const int diameter = std::max(
        1,
        static_cast<int>(std::lround(std::clamp(settings_.drawCursorSize, kMinDrawCursorDiameterPixels, kMaxDrawCursorDiameterPixels))));
    const std::uint32_t color = palette_.empty() ? 0xFF000000U : palette_.at(std::min(colorIndex_, palette_.size() - 1U));
    if (drawCursor_ != nullptr && drawCursorDiameter_ == diameter && drawCursorColor_ == color) {
        return drawCursor_;
    }

    ReleaseDrawCursor();
    drawCursor_ = CreateBrushCursor(diameter, ToColorRef(color));
    drawCursorDiameter_ = drawCursor_ != nullptr ? diameter : 0;
    drawCursorColor_ = drawCursor_ != nullptr ? color : 0;
    return drawCursor_;
}

void App::ReleaseEraserCursor() {
    if (eraserCursor_ != nullptr) {
        DestroyCursor(eraserCursor_);
        eraserCursor_ = nullptr;
    }
    eraserCursorDiameter_ = 0;
}

void App::ReleaseDrawCursor() {
    if (drawCursor_ != nullptr) {
        DestroyCursor(drawCursor_);
        drawCursor_ = nullptr;
    }
    drawCursorDiameter_ = 0;
    drawCursorColor_ = 0;
}

void App::PaintQuickSizePopup(HDC hdc, RECT clientRect) const {
    if (!quickSizePopupTool_.has_value()) {
        return;
    }

    const RECT canvasRect = CanvasRect(clientRect);
    const UINT dpi = WindowDpi(window_);
    const Tool popupTool = *quickSizePopupTool_;
    const float previewDiameter = QuickSizePreviewDiameterForTool(popupTool, clientRect);
    const QuickSizePopupLayout popupLayout = BuildQuickSizePopupLayout(
        canvasRect,
        quickSizePopupAnchor_,
        dpi,
        QuickSizePreviewExtentForTool(popupTool, clientRect));
    const COLORREF primaryText = RGB(236, 240, 245);
    const COLORREF accent = ui::kAccent;

    PaintRoundedPanel(hdc, popupLayout.panelRect, ui::kPanelFill, ui::kPanelBorder);

    HFONT labelFont = CreateFontW(FontHeightForDpi(13, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HGDIOBJ previousFont = SelectObject(hdc, labelFont);
    SetBkMode(hdc, TRANSPARENT);

    const float brushSize = BrushSizeForTool(popupTool);
    const int previewCenterX = (popupLayout.previewRect.left + popupLayout.previewRect.right) / 2;
    const int previewCenterY = (popupLayout.previewRect.top + popupLayout.previewRect.bottom) / 2;
    const Gdiplus::REAL previewLeft = static_cast<Gdiplus::REAL>(previewCenterX) - (static_cast<Gdiplus::REAL>(previewDiameter) * 0.5f);
    const Gdiplus::REAL previewTop = static_cast<Gdiplus::REAL>(previewCenterY) - (static_cast<Gdiplus::REAL>(previewDiameter) * 0.5f);
    const COLORREF previewFill = popupTool == Tool::Erase ? RGB(255, 255, 255) : ToColorRef(palette_[colorIndex_]);
    const BYTE previewAlpha = popupTool == Tool::Erase ? kEraserPreviewAlpha : 255;
    {
        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::SolidBrush brush(Gdiplus::Color(previewAlpha, GetRValue(previewFill), GetGValue(previewFill), GetBValue(previewFill)));
        Gdiplus::Pen eraserOutline(
            Gdiplus::Color(255, 0, 0, 0),
            std::max(0.5f, ScaleForDpi(kEraserOutlinePixels, dpi)));
        if (popupTool == Tool::Erase) {
            graphics.FillRectangle(
                &brush,
                previewLeft,
                previewTop,
                static_cast<Gdiplus::REAL>(previewDiameter),
                static_cast<Gdiplus::REAL>(previewDiameter));
            graphics.DrawRectangle(
                &eraserOutline,
                previewLeft,
                previewTop,
                static_cast<Gdiplus::REAL>(previewDiameter),
                static_cast<Gdiplus::REAL>(previewDiameter));
        } else {
            graphics.FillEllipse(
                &brush,
                previewLeft,
                previewTop,
                static_cast<Gdiplus::REAL>(previewDiameter),
                static_cast<Gdiplus::REAL>(previewDiameter));
        }
    }

    const RECT sliderTrackRect = ToolbarBrushTrackRect(popupLayout.sliderRect, dpi);
    const int sliderCenterY = (popupLayout.sliderRect.top + popupLayout.sliderRect.bottom) / 2;
    FillRoundedRect(hdc, sliderTrackRect, RGB(41, 51, 66));
    const int thumbCenterX = ToolbarBrushThumbCenterX(sliderTrackRect, brushSize);
    RECT sliderFillRect = sliderTrackRect;
    sliderFillRect.right = static_cast<LONG>(std::max<int>(sliderFillRect.left + 1, thumbCenterX + 1));
    FillRoundedRect(hdc, sliderFillRect, accent);
    const int thumbHalfSize = QuickSizeThumbHalfSize(dpi);
    RECT thumbRect = MakeRect(
        thumbCenterX - thumbHalfSize,
        sliderCenterY - thumbHalfSize,
        thumbCenterX + thumbHalfSize + 1,
        sliderCenterY + thumbHalfSize + 1);
    FillRoundedRect(hdc, thumbRect, RGB(244, 247, 252));
    StrokeRoundedRect(hdc, thumbRect, toolbarBrushSliderDragging_ ? RGB(255, 255, 255) : RGB(120, 138, 162), std::max(1, ScaleForDpi(1, dpi)));

    const std::wstring brushValue = ToolbarBrushValueLabel();
    RECT brushValueTextRect = popupLayout.valueRect;
    SetTextColor(hdc, primaryText);
    if (toolbarBrushSizeEditing_) {
        StrokeRoundedRect(hdc, popupLayout.valueRect, accent, std::max(1, ScaleForDpi(1, dpi)));
        brushValueTextRect.left += ScaleForDpi(kQuickSizeValuePaddingDip, dpi);
        brushValueTextRect.right -= ScaleForDpi(kQuickSizeValuePaddingDip, dpi);
        SetTextColor(hdc, RGB(244, 247, 252));
        DrawTextW(hdc, brushValue.c_str(), -1, &brushValueTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SIZE textSize {};
        if (GetTextExtentPoint32W(hdc, brushValue.c_str(), static_cast<int>(brushValue.size()), &textSize) != 0) {
            const int caretX = std::min(brushValueTextRect.right - 1, brushValueTextRect.left + textSize.cx + 1);
            PaintVerticalCaret(
                hdc,
                caretX,
                popupLayout.valueRect.top + ScaleForDpi(kQuickSizeCaretInsetDip, dpi),
                popupLayout.valueRect.bottom - ScaleForDpi(kQuickSizeCaretInsetDip, dpi),
                RGB(244, 247, 252));
        }
    } else {
        DrawTextW(hdc, brushValue.c_str(), -1, &brushValueTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, previousFont);
    DeleteObject(labelFont);
}

void App::PaintTextFormatPopup(HDC hdc, RECT clientRect) const {
    if (!textFormatPopupOpen_ || !editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || !HasRect(textEditBounds_)) {
        return;
    }
    if (*editingTextPageIndex_ >= document_.PageCount()) {
        return;
    }

    const Page& page = document_.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    const PageText& text = page.texts[*editingTextIndex_];
    const UINT dpi = WindowDpi(window_);
    const RECT canvasRect = CanvasRect(clientRect);
    const TextFormatPopupLayout layout = BuildTextFormatPopupLayout(canvasRect, textEditBounds_, textFormatFontMenuOpen_, dpi);
    const COLORREF buttonFill = RGB(29, 37, 48);
    const COLORREF buttonBorder = RGB(58, 71, 88);
    const COLORREF primaryText = RGB(236, 240, 245);
    const COLORREF mutedText = RGB(177, 188, 202);
    const COLORREF accentFill = RGB(44, 79, 119);
    const COLORREF accentBorder = ui::kAccent;
    const std::wstring fontSizeLabel = TextFormatFontSizeLabel();

    auto fontFamilyLabel = [](TextFontFamily family) -> const wchar_t* {
        switch (family) {
        case TextFontFamily::Serif:
            return L"Serif";
        case TextFontFamily::Monospace:
            return L"Monospace";
        case TextFontFamily::Sans:
        default:
            return L"Sans";
        }
    };

    auto alignmentIconFileName = [](TextAlignment alignment) -> const wchar_t* {
        switch (alignment) {
        case TextAlignment::Center:
            return L"text/cent-or.svg";
        case TextAlignment::Right:
            return L"text/right-or.svg";
        case TextAlignment::Left:
        default:
            return L"text/left-or.svg";
        }
    };

    auto alignmentFallbackLabel = [](TextAlignment alignment) -> const wchar_t* {
        switch (alignment) {
        case TextAlignment::Center:
            return L"C";
        case TextAlignment::Right:
            return L"R";
        case TextAlignment::Left:
        default:
            return L"L";
        }
    };

    PaintRoundedPanel(hdc, layout.panelRect, ui::kPanelFill, ui::kPanelBorder);

    HFONT bodyFont = CreateFontW(FontHeightForDpi(13, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ previousFont = SelectObject(hdc, bodyFont);
    SetBkMode(hdc, TRANSPARENT);

    const auto drawButton = [&](const RECT& rect, const wchar_t* iconFileName, const wchar_t* fallbackLabel, bool active) {
        const COLORREF iconColor = active ? primaryText : mutedText;
        PaintRoundedSvgButton(hdc, rect, active ? accentFill : buttonFill, active ? accentBorder : buttonBorder, iconColor, iconFileName, fallbackLabel, bodyFont, dpi);
    };

    drawButton(layout.alignCycleRect, alignmentIconFileName(text.alignment), alignmentFallbackLabel(text.alignment), true);
    drawButton(layout.boldRect, L"text/bold.svg", L"B", text.bold);
    drawButton(layout.italicRect, L"text/italics.svg", L"I", text.italic);
    drawButton(layout.underlineRect, L"text/underline.svg", L"U", text.underline);

    FillRoundedRect(hdc, layout.textSizeInputRect, buttonFill);
    StrokeRoundedRect(hdc, layout.textSizeInputRect, textFormatSizeEditing_ ? accentBorder : buttonBorder, 1);
    RECT sizeTextRect = layout.textSizeInputRect;
    sizeTextRect.left += ScaleForDpi(8, dpi);
    sizeTextRect.right -= ScaleForDpi(8, dpi);
    SetTextColor(hdc, primaryText);
    DrawTextW(hdc, fontSizeLabel.c_str(), -1, &sizeTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (textFormatSizeEditing_) {
        SIZE textSize {};
        if (GetTextExtentPoint32W(hdc, fontSizeLabel.c_str(), static_cast<int>(fontSizeLabel.size()), &textSize) != 0) {
            const int caretX = std::min(sizeTextRect.right - 1, sizeTextRect.left + textSize.cx + 1);
            RECT caretRect = MakeRect(caretX, layout.textSizeInputRect.top + ScaleForDpi(6, dpi), caretX + 1, layout.textSizeInputRect.bottom - ScaleForDpi(6, dpi));
            FillSolidRect(hdc, caretRect, primaryText);
        }
    }

    FillRoundedRect(hdc, layout.fontDropdownRect, buttonFill);
    StrokeRoundedRect(hdc, layout.fontDropdownRect, textFormatFontMenuOpen_ ? accentBorder : buttonBorder, 1);
    SetTextColor(hdc, primaryText);
    RECT dropdownTextRect = layout.fontDropdownRect;
    dropdownTextRect.left += ScaleForDpi(10, dpi);
    dropdownTextRect.right -= ScaleForDpi(10, dpi);
    DrawTextW(hdc, fontFamilyLabel(text.fontFamily), -1, &dropdownTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (textFormatFontMenuOpen_ && HasRect(layout.fontMenuRect)) {
        PaintRoundedPanel(hdc, layout.fontMenuRect, ui::kPanelFill, ui::kPanelBorder);
        for (std::size_t index = 0; index < layout.fontOptionRects.size(); ++index) {
            TextFontFamily optionFamily = TextFontFamily::Sans;
            switch (index) {
            case 1:
                optionFamily = TextFontFamily::Serif;
                break;
            case 2:
                optionFamily = TextFontFamily::Monospace;
                break;
            default:
                optionFamily = TextFontFamily::Sans;
                break;
            }

            const bool active = text.fontFamily == optionFamily;
            FillRoundedRect(hdc, layout.fontOptionRects[index], active ? accentFill : ui::kPanelFill);
            SetTextColor(hdc, active ? primaryText : mutedText);
            RECT optionTextRect = layout.fontOptionRects[index];
            optionTextRect.left += ScaleForDpi(8, dpi);
            optionTextRect.right -= ScaleForDpi(8, dpi);
            DrawTextW(hdc, fontFamilyLabel(optionFamily), -1, &optionTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    SelectObject(hdc, previousFont);
    if (bodyFont != nullptr) {
        DeleteObject(bodyFont);
    }
}

void App::PaintCanvas(HDC hdc, RECT clientRect) {
    const RECT canvasRect = CanvasRect(clientRect);
    RECT clipRect {};
    GetClipBox(hdc, &clipRect);

    HBRUSH canvasBrush = CreateSolidBrush(RGB(22, 26, 33));
    FillRect(hdc, &canvasRect, canvasBrush);
    DeleteObject(canvasBrush);

    SaveDC(hdc);
    IntersectClipRect(hdc, canvasRect.left, canvasRect.top, canvasRect.right, canvasRect.bottom);
    GetClipBox(hdc, &clipRect);
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }
        const int paintPadding = zoomSessionActive_ ? 0 : 20;
        if (pageView.rect.bottom < canvasRect.top - paintPadding || pageView.rect.top > canvasRect.bottom + paintPadding) {
            continue;
        }
        RECT visibleRect {};
        if (IntersectRect(&visibleRect, &pageView.rect, &clipRect) == 0) {
            continue;
        }
        PaintPage(hdc, pageView);
    }
    RestoreDC(hdc, -1);
    PaintSelectionOverlay(hdc, canvasRect);
}

bool App::PaintNativePdfBackground(HDC hdc, const Page& page, const RECT& destinationRect) const {
    if (hdc == nullptr || destinationRect.right <= destinationRect.left || destinationRect.bottom <= destinationRect.top) {
        return false;
    }

    const Document::PdfSource* pdfSource = document_.PdfSourceAtIndex(page.sourcePdfSourceIndex);
    if (pdfSource == nullptr || page.sourcePdfPageIndex < 0) {
        return false;
    }

    return pdfBackend_.RenderPageToHdc(
        pdfSource->pdfData,
        static_cast<std::size_t>(page.sourcePdfPageIndex),
        hdc,
        destinationRect.left,
        destinationRect.top,
        destinationRect.right - destinationRect.left,
        destinationRect.bottom - destinationRect.top);
}

bool App::TryAppendStrokeToPageSurfaceCache(std::size_t pageIndex, const Stroke& stroke) {
    if (pageIndex >= pageSurfaceCaches_.size() || stroke.points.empty()) {
        return false;
    }

    const Page& page = document_.PageAt(pageIndex);
    PageSurfaceCache& cache = pageSurfaceCaches_[pageIndex];
    if (!cache.valid || cache.pixels.empty() || cache.width <= 0 || cache.height <= 0) {
        return false;
    }

    if (cache.pageIndex != pageIndex
        || cache.strokeCount + 1U != page.strokes.size()
        || cache.imageCount != page.images.size()
        || cache.textCount != page.texts.size()
        || cache.textSignature != PageTextSurfaceSignature(page)
        || cache.rasterWidth != page.raster.width
        || cache.rasterHeight != page.raster.height
        || std::abs(cache.rasterScale - page.raster.scale) >= 0.001f
        || cache.sourcePdfSourceIndex != page.sourcePdfSourceIndex
        || cache.sourcePdfPageIndex != page.sourcePdfPageIndex) {
        return false;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = cache.width;
    bitmapInfo.bmiHeader.biHeight = -cache.height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HDC cacheDc = CreateCompatibleDC(nullptr);
    if (cacheDc == nullptr) {
        return false;
    }

    void* bits = nullptr;
    HBITMAP cacheBitmap = CreateDIBSection(cacheDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (cacheBitmap == nullptr || bits == nullptr) {
        if (cacheBitmap != nullptr) {
            DeleteObject(cacheBitmap);
        }
        DeleteDC(cacheDc);
        return false;
    }

    HGDIOBJ previousBitmap = SelectObject(cacheDc, cacheBitmap);
    std::copy(cache.pixels.begin(), cache.pixels.end(), static_cast<std::uint8_t*>(bits));

    const PageView cacheView {
        MakeRect(0, 0, cache.width, cache.height),
        pageIndex,
        cache.pageScale > 0.0f ? cache.pageScale : 1.0f,
        true,
    };
    PaintStroke(cacheDc, stroke, cacheView, false);

    std::copy_n(static_cast<const std::uint8_t*>(bits), cache.pixels.size(), cache.pixels.data());
    cache.strokeCount = page.strokes.size();
    cache.pageScale = cacheView.scale;

    SelectObject(cacheDc, previousBitmap);
    DeleteObject(cacheBitmap);
    DeleteDC(cacheDc);
    return true;
}

bool App::TryRefreshPageSurfaceCacheForStroke(std::size_t pageIndex, const Stroke& stroke) {
    if (pageIndex >= pageSurfaceCaches_.size()) {
        return false;
    }

    const PageSurfaceCache& cache = pageSurfaceCaches_[pageIndex];
    if (!cache.valid || cache.pageScale <= 0.0f) {
        return false;
    }

    const std::optional<RECT> strokeBounds = StrokeBoundsRect(stroke, cache.pageScale, 8);
    if (!strokeBounds.has_value()) {
        return false;
    }

    return TryRefreshPageSurfaceCacheRegion(pageIndex, *strokeBounds);
}

bool App::TryRefreshPageSurfaceCacheRegion(std::size_t pageIndex, RECT dirtyRect) {
    if (pageIndex >= pageSurfaceCaches_.size() || !HasRect(dirtyRect)) {
        return false;
    }

    const Page& page = document_.PageAt(pageIndex);
    PageSurfaceCache& cache = pageSurfaceCaches_[pageIndex];
    if (!cache.valid
        || cache.pixels.empty()
        || cache.width <= 0
        || cache.height <= 0
        || cache.pageIndex != pageIndex
        || cache.imageCount != page.images.size()
        || cache.textCount != page.texts.size()
        || cache.textSignature != PageTextSurfaceSignature(page)
        || cache.rasterWidth != page.raster.width
        || cache.rasterHeight != page.raster.height
        || std::abs(cache.rasterScale - page.raster.scale) >= 0.001f
        || cache.sourcePdfSourceIndex != page.sourcePdfSourceIndex
        || cache.sourcePdfPageIndex != page.sourcePdfPageIndex
        || cache.pageScale <= 0.0f) {
        return false;
    }

    RECT cacheBounds = MakeRect(0, 0, cache.width, cache.height);
    RECT clippedDirtyRect {};
    if (IntersectRect(&clippedDirtyRect, &dirtyRect, &cacheBounds) == 0) {
        return true;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = cache.width;
    bitmapInfo.bmiHeader.biHeight = -cache.height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HDC cacheDc = CreateCompatibleDC(nullptr);
    if (cacheDc == nullptr) {
        return false;
    }

    void* bits = nullptr;
    HBITMAP cacheBitmap = CreateTopDown32BitDibSection(cacheDc, cache.width, cache.height, &bits);
    if (cacheBitmap == nullptr || bits == nullptr) {
        if (cacheBitmap != nullptr) {
            DeleteObject(cacheBitmap);
        }
        DeleteDC(cacheDc);
        return false;
    }

    HGDIOBJ previousBitmap = SelectObject(cacheDc, cacheBitmap);
    std::copy(cache.pixels.begin(), cache.pixels.end(), static_cast<std::uint8_t*>(bits));

    SaveDC(cacheDc);
    IntersectClipRect(cacheDc, clippedDirtyRect.left, clippedDirtyRect.top, clippedDirtyRect.right, clippedDirtyRect.bottom);

    HBRUSH pageBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(cacheDc, &clippedDirtyRect, pageBrush);
    DeleteObject(pageBrush);

    if (!PaintNativePdfBackground(cacheDc, page, cacheBounds)
        && !page.raster.pixels.empty()
        && page.raster.width > 0
        && page.raster.height > 0) {
        BITMAPINFO pageBitmapInfo {};
        pageBitmapInfo.bmiHeader.biSize = sizeof(pageBitmapInfo.bmiHeader);
        pageBitmapInfo.bmiHeader.biWidth = page.raster.width;
        pageBitmapInfo.bmiHeader.biHeight = -page.raster.height;
        pageBitmapInfo.bmiHeader.biPlanes = 1;
        pageBitmapInfo.bmiHeader.biBitCount = 32;
        pageBitmapInfo.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(cacheDc, HALFTONE);
        StretchDIBits(
            cacheDc,
            0,
            0,
            cache.width,
            cache.height,
            0,
            0,
            page.raster.width,
            page.raster.height,
            page.raster.pixels.data(),
            &pageBitmapInfo,
            DIB_RGB_COLORS,
            SRCCOPY);
    }

    const PageView cacheView {
        MakeRect(0, 0, cache.width, cache.height),
        pageIndex,
        cache.pageScale,
        true,
    };
    const RECT dirtyPageRect = MakeRectFromFloatBounds(
        static_cast<float>(clippedDirtyRect.left) / cache.pageScale,
        static_cast<float>(clippedDirtyRect.top) / cache.pageScale,
        static_cast<float>(clippedDirtyRect.right) / cache.pageScale,
        static_cast<float>(clippedDirtyRect.bottom) / cache.pageScale);

    Gdiplus::Graphics imageGraphics(cacheDc);
    imageGraphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    imageGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    imageGraphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    for (const PageImage& image : page.images) {
        if (image.pixels.empty() || image.pixelWidth <= 0 || image.pixelHeight <= 0) {
            continue;
        }

        const auto corners = ImageCorners(image);
        float minX = corners[0].x;
        float minY = corners[0].y;
        float maxX = corners[0].x;
        float maxY = corners[0].y;
        for (const Vec2& corner : corners) {
            minX = std::min(minX, corner.x);
            minY = std::min(minY, corner.y);
            maxX = std::max(maxX, corner.x);
            maxY = std::max(maxY, corner.y);
        }
        const RECT imageBounds = MakeRectFromFloatBounds(minX, minY, maxX, maxY);
        if (!RectsOverlap(imageBounds, dirtyPageRect)) {
            continue;
        }

        Gdiplus::Bitmap bitmap(
            image.pixelWidth,
            image.pixelHeight,
            image.stride,
            PixelFormat32bppRGB,
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(image.pixels.data())));
        const Gdiplus::REAL centerX = static_cast<Gdiplus::REAL>((image.x + (image.width * 0.5f)) * cacheView.scale);
        const Gdiplus::REAL centerY = static_cast<Gdiplus::REAL>((image.y + (image.height * 0.5f)) * cacheView.scale);
        const Gdiplus::REAL drawWidth = static_cast<Gdiplus::REAL>(image.width * cacheView.scale);
        const Gdiplus::REAL drawHeight = static_cast<Gdiplus::REAL>(image.height * cacheView.scale);

        imageGraphics.ResetTransform();
        imageGraphics.TranslateTransform(centerX, centerY);
        imageGraphics.RotateTransform(image.rotationDegrees);
        imageGraphics.DrawImage(&bitmap, -drawWidth * 0.5f, -drawHeight * 0.5f, drawWidth, drawHeight);
    }
    imageGraphics.ResetTransform();
    for (std::size_t textIndex = 0; textIndex < page.texts.size(); ++textIndex) {
        if (IsEditingText(pageIndex, textIndex)) {
            continue;
        }
        const PageText& text = page.texts[textIndex];
        const auto textBounds = PageTextBoundsRect(text, page.width, 1.0f, 0);
        if (!textBounds.has_value() || !RectsOverlap(*textBounds, dirtyPageRect)) {
            continue;
        }
        PaintText(cacheDc, text, cacheView);
    }
    for (const Stroke& pageStroke : page.strokes) {
        const std::optional<RECT> strokeBounds = StrokeBoundsRect(pageStroke, 1.0f, 0);
        if (strokeBounds.has_value() && !RectsOverlap(*strokeBounds, dirtyPageRect)) {
            continue;
        }
        PaintStroke(cacheDc, pageStroke, cacheView, false);
    }

    RestoreDC(cacheDc, -1);
    std::copy_n(static_cast<const std::uint8_t*>(bits), cache.pixels.size(), cache.pixels.data());
    cache.strokeCount = page.strokes.size();
    cache.imageCount = page.images.size();
    cache.textCount = page.texts.size();
    cache.textSignature = PageTextSurfaceSignature(page);

    SelectObject(cacheDc, previousBitmap);
    DeleteObject(cacheBitmap);
    DeleteDC(cacheDc);
    return true;
}

void App::EnsurePageSurfaceCache(const PageView& pageView) {
    if (pageView.pageIndex >= pageSurfaceCaches_.size()) {
        pageSurfaceCaches_.resize(document_.PageCount());
    }

    const Page& page = document_.PageAt(pageView.pageIndex);
    PageSurfaceCache& cache = pageSurfaceCaches_[pageView.pageIndex];
    const float cacheScale = std::max(0.1f, pageView.scale);
    const std::uint64_t textSignature = PageTextSurfaceSignature(page);
    const int width = std::max(1, static_cast<int>(std::lround(std::max(1.0f, page.width) * cacheScale)));
    const int height = std::max(1, static_cast<int>(std::lround(std::max(1.0f, page.height) * cacheScale)));
    if (width <= 0 || height <= 0) {
        return;
    }

    const bool cacheMatches = cache.valid
        && cache.pageIndex == pageView.pageIndex
        && cache.width == width
        && cache.height == height
        && cache.strokeCount == page.strokes.size()
        && cache.imageCount == page.images.size()
        && cache.textCount == page.texts.size()
        && cache.textSignature == textSignature
        && cache.rasterWidth == page.raster.width
        && cache.rasterHeight == page.raster.height
        && std::abs(cache.rasterScale - page.raster.scale) < 0.001f
        && cache.sourcePdfSourceIndex == page.sourcePdfSourceIndex
        && cache.sourcePdfPageIndex == page.sourcePdfPageIndex
        && std::abs(cache.pageScale - cacheScale) < 0.001f
        && !cache.pixels.empty();
    if (cacheMatches) {
        return;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HDC cacheDc = CreateCompatibleDC(nullptr);
    if (cacheDc == nullptr) {
        return;
    }

    void* bits = nullptr;
    HBITMAP cacheBitmap = CreateDIBSection(cacheDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (cacheBitmap == nullptr || bits == nullptr) {
        if (cacheBitmap != nullptr) {
            DeleteObject(cacheBitmap);
        }
        DeleteDC(cacheDc);
        return;
    }

    HGDIOBJ previousBitmap = SelectObject(cacheDc, cacheBitmap);

    RECT cacheRect = MakeRect(0, 0, width, height);
    HBRUSH pageBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(cacheDc, &cacheRect, pageBrush);
    DeleteObject(pageBrush);

    if (!PaintNativePdfBackground(cacheDc, page, cacheRect)
        && !page.raster.pixels.empty()
        && page.raster.width > 0
        && page.raster.height > 0) {
        BITMAPINFO pageBitmapInfo {};
        pageBitmapInfo.bmiHeader.biSize = sizeof(pageBitmapInfo.bmiHeader);
        pageBitmapInfo.bmiHeader.biWidth = page.raster.width;
        pageBitmapInfo.bmiHeader.biHeight = -page.raster.height;
        pageBitmapInfo.bmiHeader.biPlanes = 1;
        pageBitmapInfo.bmiHeader.biBitCount = 32;
        pageBitmapInfo.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(cacheDc, HALFTONE);
        StretchDIBits(
            cacheDc,
            0,
            0,
            width,
            height,
            0,
            0,
            page.raster.width,
            page.raster.height,
            page.raster.pixels.data(),
            &pageBitmapInfo,
            DIB_RGB_COLORS,
            SRCCOPY);
    }

    const PageView cacheView {
        MakeRect(0, 0, width, height),
        pageView.pageIndex,
        cacheScale,
        true,
    };
    Gdiplus::Graphics imageGraphics(cacheDc);
    imageGraphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    imageGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    imageGraphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    for (const PageImage& image : page.images) {
        if (image.pixels.empty() || image.pixelWidth <= 0 || image.pixelHeight <= 0) {
            continue;
        }

        Gdiplus::Bitmap bitmap(
            image.pixelWidth,
            image.pixelHeight,
            image.stride,
            PixelFormat32bppRGB,
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(image.pixels.data())));
        const Gdiplus::REAL centerX = static_cast<Gdiplus::REAL>((image.x + (image.width * 0.5f)) * cacheView.scale);
        const Gdiplus::REAL centerY = static_cast<Gdiplus::REAL>((image.y + (image.height * 0.5f)) * cacheView.scale);
        const Gdiplus::REAL drawWidth = static_cast<Gdiplus::REAL>(image.width * cacheView.scale);
        const Gdiplus::REAL drawHeight = static_cast<Gdiplus::REAL>(image.height * cacheView.scale);

        imageGraphics.ResetTransform();
        imageGraphics.TranslateTransform(centerX, centerY);
        imageGraphics.RotateTransform(image.rotationDegrees);
        imageGraphics.DrawImage(&bitmap, -drawWidth * 0.5f, -drawHeight * 0.5f, drawWidth, drawHeight);
    }
    imageGraphics.ResetTransform();
    for (std::size_t textIndex = 0; textIndex < page.texts.size(); ++textIndex) {
        if (IsEditingText(pageView.pageIndex, textIndex)) {
            continue;
        }
        const PageText& text = page.texts[textIndex];
        PaintText(cacheDc, text, cacheView);
    }
    for (const Stroke& stroke : page.strokes) {
        PaintStroke(cacheDc, stroke, cacheView, false);
    }

    const int stride = width * 4;
    const std::size_t totalBytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    cache.pixels.resize(totalBytes);
    std::copy_n(static_cast<const std::uint8_t*>(bits), totalBytes, cache.pixels.data());
    cache.width = width;
    cache.height = height;
    cache.stride = stride;
    cache.pageIndex = pageView.pageIndex;
    cache.strokeCount = page.strokes.size();
    cache.imageCount = page.images.size();
    cache.textCount = page.texts.size();
    cache.textSignature = textSignature;
    cache.rasterWidth = page.raster.width;
    cache.rasterHeight = page.raster.height;
    cache.rasterScale = page.raster.scale;
    cache.sourcePdfSourceIndex = page.sourcePdfSourceIndex;
    cache.sourcePdfPageIndex = page.sourcePdfPageIndex;
    cache.pageScale = cacheScale;
    cache.valid = true;

    SelectObject(cacheDc, previousBitmap);
    DeleteObject(cacheBitmap);
    DeleteDC(cacheDc);
}


void App::RebuildMoveSelectionTextCache() {
    moveSelectionTextCache_.assign(document_.PageCount(), {});
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (pageIndex >= selectedTextIndices_.size()) {
            continue;
        }

        const Page& page = document_.PageAt(pageIndex);
        auto& pageCache = moveSelectionTextCache_[pageIndex];
        pageCache.reserve(selectedTextIndices_[pageIndex].size());
        for (std::size_t textIndex : selectedTextIndices_[pageIndex]) {
            if (textIndex >= page.texts.size()) {
                continue;
            }

            const PageTextBounds bounds = MeasurePageTextBounds(page.texts[textIndex], page.width);
            pageCache.push_back({
                textIndex,
                (bounds.left + bounds.right) * 0.5f,
                (bounds.top + bounds.bottom) * 0.5f,
                std::max(1.0f, bounds.right - bounds.left),
                std::max(1.0f, bounds.bottom - bounds.top),
            });
        }
    }
}

bool App::ShouldSkipStrokeInInteractionSnapshot(std::size_t pageIndex, std::size_t strokeIndex) const {
    if (interactionPreviewMode_ == InteractionPreviewMode::None) {
        return false;
    }

    if (interactionPreviewMode_ == InteractionPreviewMode::MoveSelection || interactionPreviewMode_ == InteractionPreviewMode::StrokeTransform) {
        return pageIndex < selectedStrokeIndices_.size()
            && std::find(selectedStrokeIndices_[pageIndex].begin(), selectedStrokeIndices_[pageIndex].end(), strokeIndex) != selectedStrokeIndices_[pageIndex].end();
    }

    return false;
}

bool App::ShouldSkipImageInInteractionSnapshot(std::size_t pageIndex, std::size_t imageIndex) const {
    if (interactionPreviewMode_ == InteractionPreviewMode::None) {
        return false;
    }

    if (interactionPreviewMode_ == InteractionPreviewMode::MoveSelection) {
        return pageIndex < selectedImageIndices_.size()
            && std::find(selectedImageIndices_[pageIndex].begin(), selectedImageIndices_[pageIndex].end(), imageIndex) != selectedImageIndices_[pageIndex].end();
    }

    if (interactionPreviewMode_ == InteractionPreviewMode::ImageTransform) {
        return transformingImagePageIndex_.has_value()
            && transformingImageIndex_.has_value()
            && *transformingImagePageIndex_ == pageIndex
            && *transformingImageIndex_ == imageIndex;
    }

    return false;
}

bool App::ShouldSkipTextInInteractionSnapshot(std::size_t pageIndex, std::size_t textIndex) const {
    if (interactionPreviewMode_ == InteractionPreviewMode::None) {
        return false;
    }

    if (interactionPreviewMode_ == InteractionPreviewMode::MoveSelection) {
        return pageIndex < selectedTextIndices_.size()
            && std::find(selectedTextIndices_[pageIndex].begin(), selectedTextIndices_[pageIndex].end(), textIndex) != selectedTextIndices_[pageIndex].end();
    }

    if (interactionPreviewMode_ == InteractionPreviewMode::TextTransform) {
        return transformingTextPageIndex_.has_value()
            && transformingTextIndex_.has_value()
            && *transformingTextPageIndex_ == pageIndex
            && *transformingTextIndex_ == textIndex;
    }

    return false;
}

void App::BuildInteractionPageSnapshot(const PageView& pageView) {
    if (!pageView.valid || pageView.pageIndex >= document_.PageCount()) {
        return;
    }

    if (pageView.pageIndex >= interactionPageSnapshots_.size()) {
        return;
    }

    const Page& page = document_.PageAt(pageView.pageIndex);
    PageSurfaceCache& cache = interactionPageSnapshots_[pageView.pageIndex];
    const float cacheScale = std::max(0.1f, pageView.scale);
    const int width = std::max(1, static_cast<int>(std::lround(std::max(1.0f, page.width) * cacheScale)));
    const int height = std::max(1, static_cast<int>(std::lround(std::max(1.0f, page.height) * cacheScale)));
    if (width <= 0 || height <= 0) {
        return;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HDC cacheDc = CreateCompatibleDC(nullptr);
    if (cacheDc == nullptr) {
        return;
    }

    void* bits = nullptr;
    HBITMAP cacheBitmap = CreateDIBSection(cacheDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (cacheBitmap == nullptr || bits == nullptr) {
        if (cacheBitmap != nullptr) {
            DeleteObject(cacheBitmap);
        }
        DeleteDC(cacheDc);
        return;
    }

    HGDIOBJ previousBitmap = SelectObject(cacheDc, cacheBitmap);
    RECT cacheRect = MakeRect(0, 0, width, height);
    HBRUSH pageBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(cacheDc, &cacheRect, pageBrush);
    DeleteObject(pageBrush);

    if (!PaintNativePdfBackground(cacheDc, page, cacheRect)
        && !page.raster.pixels.empty()
        && page.raster.width > 0
        && page.raster.height > 0) {
        BITMAPINFO pageBitmapInfo {};
        pageBitmapInfo.bmiHeader.biSize = sizeof(pageBitmapInfo.bmiHeader);
        pageBitmapInfo.bmiHeader.biWidth = page.raster.width;
        pageBitmapInfo.bmiHeader.biHeight = -page.raster.height;
        pageBitmapInfo.bmiHeader.biPlanes = 1;
        pageBitmapInfo.bmiHeader.biBitCount = 32;
        pageBitmapInfo.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(cacheDc, HALFTONE);
        StretchDIBits(
            cacheDc,
            0,
            0,
            width,
            height,
            0,
            0,
            page.raster.width,
            page.raster.height,
            page.raster.pixels.data(),
            &pageBitmapInfo,
            DIB_RGB_COLORS,
            SRCCOPY);
    }

    const PageView cacheView {
        MakeRect(0, 0, width, height),
        pageView.pageIndex,
        cacheScale,
        true,
    };
    Gdiplus::Graphics imageGraphics(cacheDc);
    imageGraphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    imageGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    imageGraphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    for (std::size_t imageIndex = 0; imageIndex < page.images.size(); ++imageIndex) {
        if (ShouldSkipImageInInteractionSnapshot(pageView.pageIndex, imageIndex)) {
            continue;
        }

        const PageImage& image = page.images[imageIndex];
        if (image.pixels.empty() || image.pixelWidth <= 0 || image.pixelHeight <= 0) {
            continue;
        }

        Gdiplus::Bitmap bitmap(
            image.pixelWidth,
            image.pixelHeight,
            image.stride,
            PixelFormat32bppRGB,
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(image.pixels.data())));
        const Gdiplus::REAL centerX = static_cast<Gdiplus::REAL>((image.x + (image.width * 0.5f)) * cacheView.scale);
        const Gdiplus::REAL centerY = static_cast<Gdiplus::REAL>((image.y + (image.height * 0.5f)) * cacheView.scale);
        const Gdiplus::REAL drawWidth = static_cast<Gdiplus::REAL>(image.width * cacheView.scale);
        const Gdiplus::REAL drawHeight = static_cast<Gdiplus::REAL>(image.height * cacheView.scale);

        imageGraphics.ResetTransform();
        imageGraphics.TranslateTransform(centerX, centerY);
        imageGraphics.RotateTransform(image.rotationDegrees);
        imageGraphics.DrawImage(&bitmap, -drawWidth * 0.5f, -drawHeight * 0.5f, drawWidth, drawHeight);
    }
    imageGraphics.ResetTransform();
    for (std::size_t textIndex = 0; textIndex < page.texts.size(); ++textIndex) {
        if (ShouldSkipTextInInteractionSnapshot(pageView.pageIndex, textIndex) || IsEditingText(pageView.pageIndex, textIndex)) {
            continue;
        }
        PaintText(cacheDc, page.texts[textIndex], cacheView);
    }
    for (std::size_t strokeIndex = 0; strokeIndex < page.strokes.size(); ++strokeIndex) {
        if (ShouldSkipStrokeInInteractionSnapshot(pageView.pageIndex, strokeIndex)) {
            continue;
        }
        PaintStroke(cacheDc, page.strokes[strokeIndex], cacheView, false);
    }

    const int stride = width * 4;
    const std::size_t totalBytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    cache.pixels.resize(totalBytes);
    std::copy_n(static_cast<const std::uint8_t*>(bits), totalBytes, cache.pixels.data());
    cache.width = width;
    cache.height = height;
    cache.stride = stride;
    cache.pageIndex = pageView.pageIndex;
    cache.strokeCount = page.strokes.size();
    cache.imageCount = page.images.size();
    cache.textCount = page.texts.size();
    cache.rasterWidth = page.raster.width;
    cache.rasterHeight = page.raster.height;
    cache.rasterScale = page.raster.scale;
    cache.sourcePdfSourceIndex = page.sourcePdfSourceIndex;
    cache.sourcePdfPageIndex = page.sourcePdfPageIndex;
    cache.pageScale = cacheScale;
    cache.valid = true;

    SelectObject(cacheDc, previousBitmap);
    DeleteObject(cacheBitmap);
    DeleteDC(cacheDc);
}

std::optional<RECT> App::InteractionPreviewBoundsForPage(std::size_t pageIndex) const {
    if (window_ == nullptr || pageIndex >= document_.PageCount()) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, pageIndex);
    if (!pageView.valid) {
        return std::nullopt;
    }

    RECT bounds {};
    bool hasBounds = false;
    const auto includeRect = [&](RECT rect) {
        bounds = hasBounds ? UnionRectangles(bounds, rect) : rect;
        hasBounds = true;
    };

    if (pageIndex < interactionPreviewStrokes_.size()) {
        for (const InteractionPreviewStroke& entry : interactionPreviewStrokes_[pageIndex]) {
            const auto rect = StrokeBoundsRect(entry.stroke, pageView.scale, 16);
            if (!rect.has_value()) {
                continue;
            }
            RECT windowRect = *rect;
            OffsetRect(&windowRect, pageView.rect.left, pageView.rect.top);
            includeRect(windowRect);
        }
    }

    if (pageIndex < interactionPreviewImages_.size()) {
        for (const InteractionPreviewImage& entry : interactionPreviewImages_[pageIndex]) {
            const auto corners = ImageCorners(entry.image);
            float minX = corners[0].x;
            float minY = corners[0].y;
            float maxX = corners[0].x;
            float maxY = corners[0].y;
            for (const Vec2& corner : corners) {
                minX = std::min(minX, corner.x);
                minY = std::min(minY, corner.y);
                maxX = std::max(maxX, corner.x);
                maxY = std::max(maxY, corner.y);
            }
            RECT windowRect = MakeRect(
                pageView.rect.left + static_cast<int>(std::floor(minX * pageView.scale)) - 20,
                pageView.rect.top + static_cast<int>(std::floor(minY * pageView.scale)) - 20,
                pageView.rect.left + static_cast<int>(std::ceil(maxX * pageView.scale)) + 21,
                pageView.rect.top + static_cast<int>(std::ceil(maxY * pageView.scale)) + 21);
            includeRect(windowRect);
        }
    }

    if (pageIndex < interactionPreviewTexts_.size()) {
        for (const InteractionPreviewText& entry : interactionPreviewTexts_[pageIndex]) {
            const auto rect = PageTextCachedBoundsRect(entry.text, entry.boundsWidth, entry.boundsHeight, pageView.scale, 16);
            if (!rect.has_value()) {
                continue;
            }
            RECT windowRect = *rect;
            OffsetRect(&windowRect, pageView.rect.left, pageView.rect.top);
            includeRect(windowRect);
        }
    }

    return hasBounds ? std::optional<RECT>(bounds) : std::nullopt;
}

void App::InvalidateInteractionPreviewDelta(const std::vector<std::optional<RECT>>& previousBounds) {
    if (window_ == nullptr) {
        return;
    }

    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        const std::optional<RECT> currentBounds = InteractionPreviewBoundsForPage(pageIndex);
        if (pageIndex < previousBounds.size() && previousBounds[pageIndex].has_value()) {
            if (currentBounds.has_value()) {
                RECT dirty = UnionRectangles(*previousBounds[pageIndex], *currentBounds);
                InvalidateRect(window_, &dirty, FALSE);
            } else {
                InvalidateRect(window_, &*previousBounds[pageIndex], FALSE);
            }
        } else if (currentBounds.has_value()) {
            InvalidateRect(window_, &*currentBounds, FALSE);
        }
    }
}

void App::RebuildMoveSelectionPreview(POINT cursor) {
    if (interactionPreviewMode_ != InteractionPreviewMode::MoveSelection || window_ == nullptr) {
        return;
    }

    std::vector<std::optional<RECT>> previousBounds(document_.PageCount());
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        previousBounds[pageIndex] = InteractionPreviewBoundsForPage(pageIndex);
    }
    interactionPreviewStrokes_.assign(document_.PageCount(), {});
    interactionPreviewTexts_.assign(document_.PageCount(), {});
    interactionPreviewImages_.assign(document_.PageCount(), {});

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const std::vector<PageView> pageViews = CalculatePageViews(clientRect);
    if (pageViews.size() != document_.PageCount()) {
        return;
    }

    const auto targetPageForScreenPoint = [&](POINT screenPoint, std::size_t fallbackPageIndex) {
        std::size_t bestPageIndex = std::min(fallbackPageIndex, pageViews.size() - 1U);
        long bestDistanceSquared = LONG_MAX;
        for (const PageView& pageView : pageViews) {
            if (!pageView.valid) {
                continue;
            }
            if (PtInRect(&pageView.rect, screenPoint) != 0) {
                return pageView.pageIndex;
            }

            const long dx = static_cast<long>(std::max<LONG>(pageView.rect.left - screenPoint.x, 0))
                + static_cast<long>(std::max<LONG>(screenPoint.x - pageView.rect.right, 0));
            const long dy = static_cast<long>(std::max<LONG>(pageView.rect.top - screenPoint.y, 0))
                + static_cast<long>(std::max<LONG>(screenPoint.y - pageView.rect.bottom, 0));
            const long distanceSquared = (dx * dx) + (dy * dy);
            if (distanceSquared < bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                bestPageIndex = pageView.pageIndex;
            }
        }
        return bestPageIndex;
    };

    const int deltaX = cursor.x - interactionPreviewStartCursor_.x;
    const int deltaY = cursor.y - interactionPreviewStartCursor_.y;

    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        const PageView& sourceView = pageViews[pageIndex];
        const Page& page = document_.PageAt(pageIndex);

        if (pageIndex < selectedStrokeIndices_.size()) {
            for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
                if (strokeIndex >= page.strokes.size()) {
                    continue;
                }
                const Stroke& originalStroke = page.strokes[strokeIndex];
                float minX = 0.0f;
                float minY = 0.0f;
                float maxX = 0.0f;
                float maxY = 0.0f;
                if (!StrokeBounds(originalStroke, &minX, &minY, &maxX, &maxY)) {
                    continue;
                }

                const POINT centerPoint {
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.left) + (((minX + maxX) * 0.5f) * sourceView.scale) + static_cast<float>(deltaX))),
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.top) + (((minY + maxY) * 0.5f) * sourceView.scale) + static_cast<float>(deltaY))),
                };
                const std::size_t targetPageIndex = targetPageForScreenPoint(centerPoint, pageIndex);
                const PageView& targetView = pageViews[targetPageIndex];

                InteractionPreviewStroke preview {};
                preview.sourcePageIndex = pageIndex;
                preview.sourceIndex = strokeIndex;
                preview.stroke = originalStroke;
                for (Vec2& point : preview.stroke.points) {
                    const float screenX = static_cast<float>(sourceView.rect.left) + (point.x * sourceView.scale) + static_cast<float>(deltaX);
                    const float screenY = static_cast<float>(sourceView.rect.top) + (point.y * sourceView.scale) + static_cast<float>(deltaY);
                    point.x = (screenX - static_cast<float>(targetView.rect.left)) / std::max(0.1f, targetView.scale);
                    point.y = (screenY - static_cast<float>(targetView.rect.top)) / std::max(0.1f, targetView.scale);
                }
                interactionPreviewStrokes_[targetPageIndex].push_back(std::move(preview));
            }
        }

        if (pageIndex < moveSelectionTextCache_.size()) {
            for (const MoveSelectionTextCacheEntry& cacheEntry : moveSelectionTextCache_[pageIndex]) {
                if (cacheEntry.textIndex >= page.texts.size()) {
                    continue;
                }
                const PageText& originalText = page.texts[cacheEntry.textIndex];
                const POINT centerPoint {
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.left) + (cacheEntry.centerX * sourceView.scale) + static_cast<float>(deltaX))),
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.top) + (cacheEntry.centerY * sourceView.scale) + static_cast<float>(deltaY))),
                };
                const std::size_t targetPageIndex = targetPageForScreenPoint(centerPoint, pageIndex);
                const PageView& targetView = pageViews[targetPageIndex];

                InteractionPreviewText preview {};
                preview.sourcePageIndex = pageIndex;
                preview.sourceIndex = cacheEntry.textIndex;
                preview.text = originalText;
                preview.boundsWidth = cacheEntry.boundsWidth;
                preview.boundsHeight = cacheEntry.boundsHeight;
                const float screenX = static_cast<float>(sourceView.rect.left) + (originalText.x * sourceView.scale) + static_cast<float>(deltaX);
                const float screenY = static_cast<float>(sourceView.rect.top) + (originalText.y * sourceView.scale) + static_cast<float>(deltaY);
                preview.text.x = (screenX - static_cast<float>(targetView.rect.left)) / std::max(0.1f, targetView.scale);
                preview.text.y = (screenY - static_cast<float>(targetView.rect.top)) / std::max(0.1f, targetView.scale);
                interactionPreviewTexts_[targetPageIndex].push_back(std::move(preview));
            }
        }

        if (pageIndex < selectedImageIndices_.size()) {
            for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
                if (imageIndex >= page.images.size()) {
                    continue;
                }
                const PageImage& originalImage = page.images[imageIndex];
                const Vec2 center = ImageCenter(originalImage);
                const POINT centerPoint {
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.left) + (center.x * sourceView.scale) + static_cast<float>(deltaX))),
                    static_cast<LONG>(std::lround(static_cast<float>(sourceView.rect.top) + (center.y * sourceView.scale) + static_cast<float>(deltaY))),
                };
                const std::size_t targetPageIndex = targetPageForScreenPoint(centerPoint, pageIndex);
                const PageView& targetView = pageViews[targetPageIndex];

                InteractionPreviewImage preview {};
                preview.sourcePageIndex = pageIndex;
                preview.sourceIndex = imageIndex;
                preview.image = originalImage;
                const float screenX = static_cast<float>(sourceView.rect.left) + (originalImage.x * sourceView.scale) + static_cast<float>(deltaX);
                const float screenY = static_cast<float>(sourceView.rect.top) + (originalImage.y * sourceView.scale) + static_cast<float>(deltaY);
                preview.image.x = (screenX - static_cast<float>(targetView.rect.left)) / std::max(0.1f, targetView.scale);
                preview.image.y = (screenY - static_cast<float>(targetView.rect.top)) / std::max(0.1f, targetView.scale);
                interactionPreviewImages_[targetPageIndex].push_back(std::move(preview));
            }
        }
    }

    InvalidateInteractionPreviewDelta(previousBounds);
}

void App::CommitMoveSelectionPreview() {
    if (interactionPreviewMode_ != InteractionPreviewMode::MoveSelection) {
        return;
    }

    std::vector<bool> affectedPages(document_.PageCount(), false);
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (pageIndex < selectedStrokeIndices_.size()) {
            auto strokeIndices = selectedStrokeIndices_[pageIndex];
            std::sort(strokeIndices.rbegin(), strokeIndices.rend());
            auto& strokes = document_.PageAt(pageIndex).strokes;
            for (std::size_t strokeIndex : strokeIndices) {
                if (strokeIndex < strokes.size()) {
                    strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(strokeIndex));
                    affectedPages[pageIndex] = true;
                }
            }
        }
        if (pageIndex < selectedImageIndices_.size()) {
            auto imageIndices = selectedImageIndices_[pageIndex];
            std::sort(imageIndices.rbegin(), imageIndices.rend());
            auto& images = document_.PageAt(pageIndex).images;
            for (std::size_t imageIndex : imageIndices) {
                if (imageIndex < images.size()) {
                    images.erase(images.begin() + static_cast<std::ptrdiff_t>(imageIndex));
                    affectedPages[pageIndex] = true;
                }
            }
        }
        if (pageIndex < selectedTextIndices_.size()) {
            auto textIndices = selectedTextIndices_[pageIndex];
            std::sort(textIndices.rbegin(), textIndices.rend());
            auto& texts = document_.PageAt(pageIndex).texts;
            for (std::size_t textIndex : textIndices) {
                if (textIndex < texts.size()) {
                    texts.erase(texts.begin() + static_cast<std::ptrdiff_t>(textIndex));
                    affectedPages[pageIndex] = true;
                }
            }
        }
    }

    ClearSelection();
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        auto& page = document_.PageAt(pageIndex);
        for (const InteractionPreviewStroke& preview : interactionPreviewStrokes_[pageIndex]) {
            page.strokes.push_back(preview.stroke);
            selectedStrokeIndices_[pageIndex].push_back(page.strokes.size() - 1U);
            affectedPages[pageIndex] = true;
        }
        for (const InteractionPreviewImage& preview : interactionPreviewImages_[pageIndex]) {
            page.images.push_back(preview.image);
            selectedImageIndices_[pageIndex].push_back(page.images.size() - 1U);
            affectedPages[pageIndex] = true;
        }
        for (const InteractionPreviewText& preview : interactionPreviewTexts_[pageIndex]) {
            page.texts.push_back(preview.text);
            selectedTextIndices_[pageIndex].push_back(page.texts.size() - 1U);
            affectedPages[pageIndex] = true;
        }
    }

    for (std::size_t pageIndex = 0; pageIndex < affectedPages.size(); ++pageIndex) {
        if (affectedPages[pageIndex]) {
            InvalidatePageSurfaceCache(pageIndex);
        }
    }
}

void App::RebuildImageTransformPreview(POINT cursor) {
    if (!transformingImagePageIndex_.has_value() || interactionPreviewMode_ != InteractionPreviewMode::ImageTransform || window_ == nullptr) {
        return;
    }

    std::vector<std::optional<RECT>> previousBounds(document_.PageCount());
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        previousBounds[pageIndex] = InteractionPreviewBoundsForPage(pageIndex);
    }
    interactionPreviewImages_.assign(document_.PageCount(), {});
    interactionPreviewStrokes_.assign(document_.PageCount(), {});

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *transformingImagePageIndex_);
    if (!pageView.valid) {
        return;
    }

    const Page& page = document_.PageAt(*transformingImagePageIndex_);
    const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const Vec2 currentPoint {
        std::clamp((static_cast<float>(cursor.x - pageView.rect.left)) / pageView.scale, 0.0f, page.width),
        std::clamp((static_cast<float>(cursor.y - pageView.rect.top)) / pageView.scale, 0.0f, page.height),
    };
    PageImage updated = imageTransformInitial_;
    const Vec2 center = ImageCenter(imageTransformInitial_);

    if (imageTransformHandle_ == ImageTransformHandle::Rotate) {
        const float currentAngle = std::atan2(currentPoint.y - center.y, currentPoint.x - center.x) * (180.0f / kPi);
        updated.rotationDegrees = imageTransformInitial_.rotationDegrees + (currentAngle - imageTransformStartAngleDegrees_);
        if (shiftPressed) {
            updated.rotationDegrees = std::round(updated.rotationDegrees / 15.0f) * 15.0f;
        }
    } else {
        const Vec2 localPoint = ToImageLocalPoint(imageTransformInitial_, currentPoint);
        float left = -imageTransformInitial_.width * 0.5f;
        float right = imageTransformInitial_.width * 0.5f;
        float top = -imageTransformInitial_.height * 0.5f;
        float bottom = imageTransformInitial_.height * 0.5f;
        const float localX = localPoint.x - center.x;
        const float localY = localPoint.y - center.y;
        constexpr float kMinImageSize = 18.0f;

        switch (imageTransformHandle_) {
        case ImageTransformHandle::ResizeNorthWest:
            left = std::min(localX, right - kMinImageSize);
            top = std::min(localY, bottom - kMinImageSize);
            break;
        case ImageTransformHandle::ResizeNorth:
            top = std::min(localY, bottom - kMinImageSize);
            break;
        case ImageTransformHandle::ResizeNorthEast:
            right = std::max(localX, left + kMinImageSize);
            top = std::min(localY, bottom - kMinImageSize);
            break;
        case ImageTransformHandle::ResizeEast:
            right = std::max(localX, left + kMinImageSize);
            break;
        case ImageTransformHandle::ResizeSouthEast:
            right = std::max(localX, left + kMinImageSize);
            bottom = std::max(localY, top + kMinImageSize);
            break;
        case ImageTransformHandle::ResizeSouth:
            bottom = std::max(localY, top + kMinImageSize);
            break;
        case ImageTransformHandle::ResizeSouthWest:
            left = std::min(localX, right - kMinImageSize);
            bottom = std::max(localY, top + kMinImageSize);
            break;
        case ImageTransformHandle::ResizeWest:
            left = std::min(localX, right - kMinImageSize);
            break;
        default:
            break;
        }

        if (shiftPressed) {
            const float initialWidth = std::max(kMinImageSize, imageTransformInitial_.width);
            const float initialHeight = std::max(kMinImageSize, imageTransformInitial_.height);
            const float aspect = initialWidth / initialHeight;
            switch (imageTransformHandle_) {
            case ImageTransformHandle::ResizeNorthWest:
            case ImageTransformHandle::ResizeNorthEast:
            case ImageTransformHandle::ResizeSouthEast:
            case ImageTransformHandle::ResizeSouthWest: {
                const float widthScale = std::abs(right - left) / initialWidth;
                const float heightScale = std::abs(bottom - top) / initialHeight;
                const float scale = std::max(widthScale, heightScale);
                const float newWidth = std::max(kMinImageSize, initialWidth * scale);
                const float newHeight = std::max(kMinImageSize, initialHeight * scale);
                if (imageTransformHandle_ == ImageTransformHandle::ResizeNorthWest) {
                    left = right - newWidth;
                    top = bottom - newHeight;
                } else if (imageTransformHandle_ == ImageTransformHandle::ResizeNorthEast) {
                    right = left + newWidth;
                    top = bottom - newHeight;
                } else if (imageTransformHandle_ == ImageTransformHandle::ResizeSouthEast) {
                    right = left + newWidth;
                    bottom = top + newHeight;
                } else {
                    left = right - newWidth;
                    bottom = top + newHeight;
                }
                break;
            }
            case ImageTransformHandle::ResizeEast:
            case ImageTransformHandle::ResizeWest: {
                const float newWidth = std::max(kMinImageSize, std::abs(right - left));
                const float newHeight = std::max(kMinImageSize, newWidth / aspect);
                if (imageTransformHandle_ == ImageTransformHandle::ResizeEast) {
                    right = left + newWidth;
                } else {
                    left = right - newWidth;
                }
                top = -newHeight * 0.5f;
                bottom = newHeight * 0.5f;
                break;
            }
            case ImageTransformHandle::ResizeNorth:
            case ImageTransformHandle::ResizeSouth: {
                const float newHeight = std::max(kMinImageSize, std::abs(bottom - top));
                const float newWidth = std::max(kMinImageSize, newHeight * aspect);
                if (imageTransformHandle_ == ImageTransformHandle::ResizeNorth) {
                    top = bottom - newHeight;
                } else {
                    bottom = top + newHeight;
                }
                left = -newWidth * 0.5f;
                right = newWidth * 0.5f;
                break;
            }
            default:
                break;
            }
        }

        const Vec2 localCenterOffset { (left + right) * 0.5f, (top + bottom) * 0.5f };
        const Vec2 newCenter = RotatePointAround(
            { center.x + localCenterOffset.x, center.y + localCenterOffset.y },
            center,
            imageTransformInitial_.rotationDegrees);
        updated.width = std::max(kMinImageSize, right - left);
        updated.height = std::max(kMinImageSize, bottom - top);
        updated.x = newCenter.x - (updated.width * 0.5f);
        updated.y = newCenter.y - (updated.height * 0.5f);
    }

    interactionPreviewImages_[*transformingImagePageIndex_].push_back({
        *transformingImagePageIndex_,
        transformingImageIndex_.value_or(0),
        std::move(updated),
    });
    InvalidateInteractionPreviewDelta(previousBounds);
}

void App::CommitImageTransformPreview() {
    if (interactionPreviewMode_ != InteractionPreviewMode::ImageTransform
        || !transformingImagePageIndex_.has_value()
        || !transformingImageIndex_.has_value()
        || *transformingImagePageIndex_ >= interactionPreviewImages_.size()
        || interactionPreviewImages_[*transformingImagePageIndex_].empty()) {
        return;
    }

    Page& page = document_.PageAt(*transformingImagePageIndex_);
    if (*transformingImageIndex_ < page.images.size()) {
        page.images[*transformingImageIndex_] = interactionPreviewImages_[*transformingImagePageIndex_].front().image;
        InvalidatePageSurfaceCache(*transformingImagePageIndex_);
    }
}

void App::RebuildStrokeTransformPreview(POINT cursor) {
    if (!transformingStrokePageIndex_.has_value() || interactionPreviewMode_ != InteractionPreviewMode::StrokeTransform || window_ == nullptr) {
        return;
    }

    std::vector<std::optional<RECT>> previousBounds(document_.PageCount());
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        previousBounds[pageIndex] = InteractionPreviewBoundsForPage(pageIndex);
    }
    interactionPreviewImages_.assign(document_.PageCount(), {});
    interactionPreviewStrokes_.assign(document_.PageCount(), {});

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *transformingStrokePageIndex_);
    if (!pageView.valid) {
        return;
    }

    const Page& page = document_.PageAt(*transformingStrokePageIndex_);
    const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const Vec2 currentPoint {
        std::clamp((static_cast<float>(cursor.x - pageView.rect.left)) / pageView.scale, 0.0f, page.width),
        std::clamp((static_cast<float>(cursor.y - pageView.rect.top)) / pageView.scale, 0.0f, page.height),
    };
    const Vec2 center {
        (strokeTransformBoundsMin_.x + strokeTransformBoundsMax_.x) * 0.5f,
        (strokeTransformBoundsMin_.y + strokeTransformBoundsMax_.y) * 0.5f,
    };

    for (const auto& [strokeIndex, originalStroke] : strokeTransformInitials_) {
        Stroke updated = originalStroke;
        if (strokeTransformHandle_ == ImageTransformHandle::Rotate) {
            float angleDelta = (std::atan2(currentPoint.y - center.y, currentPoint.x - center.x) * (180.0f / kPi)) - strokeTransformStartAngleDegrees_;
            if (shiftPressed) {
                angleDelta = std::round(angleDelta / 15.0f) * 15.0f;
            }
            for (Vec2& point : updated.points) {
                point = RotatePointAround(point, center, angleDelta);
            }
        } else {
            float left = strokeTransformBoundsMin_.x - center.x;
            float right = strokeTransformBoundsMax_.x - center.x;
            float top = strokeTransformBoundsMin_.y - center.y;
            float bottom = strokeTransformBoundsMax_.y - center.y;
            const float localX = currentPoint.x - center.x;
            const float localY = currentPoint.y - center.y;
            constexpr float kMinStrokeSize = 12.0f;

            switch (strokeTransformHandle_) {
            case ImageTransformHandle::ResizeNorthWest:
                left = std::min(localX, right - kMinStrokeSize);
                top = std::min(localY, bottom - kMinStrokeSize);
                break;
            case ImageTransformHandle::ResizeNorth:
                top = std::min(localY, bottom - kMinStrokeSize);
                break;
            case ImageTransformHandle::ResizeNorthEast:
                right = std::max(localX, left + kMinStrokeSize);
                top = std::min(localY, bottom - kMinStrokeSize);
                break;
            case ImageTransformHandle::ResizeEast:
                right = std::max(localX, left + kMinStrokeSize);
                break;
            case ImageTransformHandle::ResizeSouthEast:
                right = std::max(localX, left + kMinStrokeSize);
                bottom = std::max(localY, top + kMinStrokeSize);
                break;
            case ImageTransformHandle::ResizeSouth:
                bottom = std::max(localY, top + kMinStrokeSize);
                break;
            case ImageTransformHandle::ResizeSouthWest:
                left = std::min(localX, right - kMinStrokeSize);
                bottom = std::max(localY, top + kMinStrokeSize);
                break;
            case ImageTransformHandle::ResizeWest:
                left = std::min(localX, right - kMinStrokeSize);
                break;
            default:
                break;
            }

            const float initialWidth = std::max(kMinStrokeSize, strokeTransformBoundsMax_.x - strokeTransformBoundsMin_.x);
            const float initialHeight = std::max(kMinStrokeSize, strokeTransformBoundsMax_.y - strokeTransformBoundsMin_.y);
            if (shiftPressed) {
                const float aspect = initialWidth / initialHeight;
                switch (strokeTransformHandle_) {
                case ImageTransformHandle::ResizeNorthWest:
                case ImageTransformHandle::ResizeNorthEast:
                case ImageTransformHandle::ResizeSouthEast:
                case ImageTransformHandle::ResizeSouthWest: {
                    const float widthScale = std::abs(right - left) / initialWidth;
                    const float heightScale = std::abs(bottom - top) / initialHeight;
                    const float scale = std::max(widthScale, heightScale);
                    const float newWidth = std::max(kMinStrokeSize, initialWidth * scale);
                    const float newHeight = std::max(kMinStrokeSize, initialHeight * scale);
                    if (strokeTransformHandle_ == ImageTransformHandle::ResizeNorthWest) {
                        left = right - newWidth;
                        top = bottom - newHeight;
                    } else if (strokeTransformHandle_ == ImageTransformHandle::ResizeNorthEast) {
                        right = left + newWidth;
                        top = bottom - newHeight;
                    } else if (strokeTransformHandle_ == ImageTransformHandle::ResizeSouthEast) {
                        right = left + newWidth;
                        bottom = top + newHeight;
                    } else {
                        left = right - newWidth;
                        bottom = top + newHeight;
                    }
                    break;
                }
                case ImageTransformHandle::ResizeEast:
                case ImageTransformHandle::ResizeWest: {
                    const float newWidth = std::max(kMinStrokeSize, std::abs(right - left));
                    const float newHeight = std::max(kMinStrokeSize, newWidth / aspect);
                    if (strokeTransformHandle_ == ImageTransformHandle::ResizeEast) {
                        right = left + newWidth;
                    } else {
                        left = right - newWidth;
                    }
                    top = -newHeight * 0.5f;
                    bottom = newHeight * 0.5f;
                    break;
                }
                case ImageTransformHandle::ResizeNorth:
                case ImageTransformHandle::ResizeSouth: {
                    const float newHeight = std::max(kMinStrokeSize, std::abs(bottom - top));
                    const float newWidth = std::max(kMinStrokeSize, newHeight * aspect);
                    if (strokeTransformHandle_ == ImageTransformHandle::ResizeNorth) {
                        top = bottom - newHeight;
                    } else {
                        bottom = top + newHeight;
                    }
                    left = -newWidth * 0.5f;
                    right = newWidth * 0.5f;
                    break;
                }
                default:
                    break;
                }
            }

            const Vec2 localCenterOffset { (left + right) * 0.5f, (top + bottom) * 0.5f };
            const float scaleX = std::max(kMinStrokeSize, right - left) / initialWidth;
            const float scaleY = std::max(kMinStrokeSize, bottom - top) / initialHeight;
            for (Vec2& point : updated.points) {
                const float localXPoint = point.x - center.x;
                const float localYPoint = point.y - center.y;
                point.x = center.x + (localXPoint * scaleX) + localCenterOffset.x;
                point.y = center.y + (localYPoint * scaleY) + localCenterOffset.y;
            }
        }

        interactionPreviewStrokes_[*transformingStrokePageIndex_].push_back({
            *transformingStrokePageIndex_,
            strokeIndex,
            std::move(updated),
        });
    }

    InvalidateInteractionPreviewDelta(previousBounds);
}

void App::CommitStrokeTransformPreview() {
    if (interactionPreviewMode_ != InteractionPreviewMode::StrokeTransform || !transformingStrokePageIndex_.has_value()) {
        return;
    }

    Page& page = document_.PageAt(*transformingStrokePageIndex_);
    for (const InteractionPreviewStroke& preview : interactionPreviewStrokes_[*transformingStrokePageIndex_]) {
        if (preview.sourceIndex < page.strokes.size()) {
            page.strokes[preview.sourceIndex] = preview.stroke;
        }
    }
    InvalidatePageSurfaceCache(*transformingStrokePageIndex_);
}

void App::ClearZoomSessionSnapshots() {
    zoomPageSnapshots_.clear();
}

void App::CaptureZoomPageSnapshot(const PageView& pageView) {
    if (!pageView.valid) {
        return;
    }

    const int width = static_cast<int>(std::max<LONG>(0, pageView.rect.right - pageView.rect.left));
    const int height = static_cast<int>(std::max<LONG>(0, pageView.rect.bottom - pageView.rect.top));
    if (width <= 0 || height <= 0) {
        return;
    }

    EnsurePageSurfaceCache(pageView);
    if (pageView.pageIndex >= pageSurfaceCaches_.size()) {
        return;
    }

    const PageSurfaceCache& cache = pageSurfaceCaches_[pageView.pageIndex];
    if (!cache.valid || cache.pixels.empty() || cache.width <= 0 || cache.height <= 0) {
        return;
    }

    if (pageView.pageIndex >= zoomPageSnapshots_.size()) {
        zoomPageSnapshots_.resize(document_.PageCount());
    }

    HDC snapshotDc = CreateCompatibleDC(nullptr);
    if (snapshotDc == nullptr) {
        return;
    }

    void* bits = nullptr;
    HBITMAP snapshotBitmap = CreateTopDown32BitDibSection(snapshotDc, width, height, &bits);
    if (snapshotBitmap == nullptr || bits == nullptr) {
        if (snapshotBitmap != nullptr) {
            DeleteObject(snapshotBitmap);
        }
        DeleteDC(snapshotDc);
        return;
    }

    HGDIOBJ previousBitmap = SelectObject(snapshotDc, snapshotBitmap);

    RECT snapshotRect = MakeRect(0, 0, width, height);
    HBRUSH pageBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(snapshotDc, &snapshotRect, pageBrush);
    DeleteObject(pageBrush);

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = cache.width;
    bitmapInfo.bmiHeader.biHeight = -cache.height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    SetStretchBltMode(snapshotDc, HALFTONE);
    StretchDIBits(
        snapshotDc,
        0,
        0,
        width,
        height,
        0,
        0,
        cache.width,
        cache.height,
        cache.pixels.data(),
        &bitmapInfo,
        DIB_RGB_COLORS,
        SRCCOPY);

    ZoomPageSnapshot& snapshot = zoomPageSnapshots_[pageView.pageIndex];
    snapshot.width = width;
    snapshot.height = height;
    snapshot.stride = width * 4;
    snapshot.pageIndex = pageView.pageIndex;
    snapshot.valid = true;
    snapshot.pixels.resize(static_cast<std::size_t>(snapshot.stride) * static_cast<std::size_t>(snapshot.height));
    std::copy_n(static_cast<const std::uint8_t*>(bits), snapshot.pixels.size(), snapshot.pixels.data());

    SelectObject(snapshotDc, previousBitmap);
    DeleteObject(snapshotBitmap);
    DeleteDC(snapshotDc);
}

void App::CaptureZoomSessionSnapshots() {
    ClearZoomSessionSnapshots();
    if (window_ == nullptr || document_.PageCount() == 0U) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT canvasRect = CanvasRect(clientRect);
    const UINT dpi = WindowDpi(window_);
    const int viewportHeight = std::max(1, static_cast<int>(canvasRect.bottom - canvasRect.top));
    const int snapshotPadding = std::max(ScaleForDpi(120, dpi), viewportHeight);

    zoomPageSnapshots_.resize(document_.PageCount());
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }
        if (pageView.rect.bottom < canvasRect.top - snapshotPadding || pageView.rect.top > canvasRect.bottom + snapshotPadding) {
            continue;
        }
        CaptureZoomPageSnapshot(pageView);
    }
}

void App::PaintPage(HDC hdc, const PageView& pageView) {
    if (!zoomSessionActive_) {
        RECT shadow = pageView.rect;
        OffsetRect(&shadow, 8, 10);
        HBRUSH shadowBrush = CreateSolidBrush(RGB(7, 10, 15));
        FillRect(hdc, &shadow, shadowBrush);
        DeleteObject(shadowBrush);
    }

    HBRUSH pageBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &pageView.rect, pageBrush);
    DeleteObject(pageBrush);

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(94, 105, 121));
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, pageView.rect.left, pageView.rect.top, pageView.rect.right, pageView.rect.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    SaveDC(hdc);
    IntersectClipRect(hdc, pageView.rect.left, pageView.rect.top, pageView.rect.right, pageView.rect.bottom);

    const Page& page = document_.PageAt(pageView.pageIndex);
    const bool pageHasPendingErase = PageHasPendingErase(pageView.pageIndex);
    const ZoomPageSnapshot* zoomSnapshot = nullptr;
    const PageSurfaceCache* interactionSnapshot = nullptr;
    bool canReuseFrozenCache = false;
    if (interactionPreviewMode_ != InteractionPreviewMode::None && pageView.pageIndex < interactionPageSnapshots_.size()) {
        const PageSurfaceCache& snapshot = interactionPageSnapshots_[pageView.pageIndex];
        if (snapshot.valid && !snapshot.pixels.empty() && snapshot.width > 0 && snapshot.height > 0) {
            interactionSnapshot = &snapshot;
            canReuseFrozenCache = true;
        }
    }
    if (zoomSessionActive_ && pageView.pageIndex < zoomPageSnapshots_.size()) {
        const ZoomPageSnapshot& snapshot = zoomPageSnapshots_[pageView.pageIndex];
        if (snapshot.valid && !snapshot.pixels.empty() && snapshot.width > 0 && snapshot.height > 0) {
            zoomSnapshot = &snapshot;
            canReuseFrozenCache = true;
        }
    }
    if (pageView.pageIndex < pageSurfaceCaches_.size()) {
        const PageSurfaceCache& cache = pageSurfaceCaches_[pageView.pageIndex];
        canReuseFrozenCache = canReuseFrozenCache || (cache.valid && !cache.pixels.empty());
    }
    if (!zoomSessionActive_ && !pageHasPendingErase) {
        EnsurePageSurfaceCache(pageView);
    }

    if (pageHasPendingErase) {
        if (!PaintNativePdfBackground(hdc, page, pageView.rect)
            && !page.raster.pixels.empty()
            && page.raster.width > 0
            && page.raster.height > 0) {
            BITMAPINFO pageBitmapInfo {};
            pageBitmapInfo.bmiHeader.biSize = sizeof(pageBitmapInfo.bmiHeader);
            pageBitmapInfo.bmiHeader.biWidth = page.raster.width;
            pageBitmapInfo.bmiHeader.biHeight = -page.raster.height;
            pageBitmapInfo.bmiHeader.biPlanes = 1;
            pageBitmapInfo.bmiHeader.biBitCount = 32;
            pageBitmapInfo.bmiHeader.biCompression = BI_RGB;

            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, nullptr);
            StretchDIBits(
                hdc,
                pageView.rect.left,
                pageView.rect.top,
                pageView.rect.right - pageView.rect.left,
                pageView.rect.bottom - pageView.rect.top,
                0,
                0,
                page.raster.width,
                page.raster.height,
                page.raster.pixels.data(),
                &pageBitmapInfo,
                DIB_RGB_COLORS,
                SRCCOPY);
        }

        Gdiplus::Graphics imageGraphics(hdc);
        imageGraphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        imageGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        imageGraphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        for (std::size_t imageIndex = 0; imageIndex < page.images.size(); ++imageIndex) {
            const PageImage& image = page.images[imageIndex];
            if (image.pixels.empty() || image.pixelWidth <= 0 || image.pixelHeight <= 0) {
                continue;
            }

            Gdiplus::Bitmap bitmap(
                image.pixelWidth,
                image.pixelHeight,
                image.stride,
                PixelFormat32bppRGB,
                const_cast<BYTE*>(reinterpret_cast<const BYTE*>(image.pixels.data())));
            const Gdiplus::REAL centerX = static_cast<Gdiplus::REAL>(pageView.rect.left + ((image.x + (image.width * 0.5f)) * pageView.scale));
            const Gdiplus::REAL centerY = static_cast<Gdiplus::REAL>(pageView.rect.top + ((image.y + (image.height * 0.5f)) * pageView.scale));
            const Gdiplus::REAL drawWidth = static_cast<Gdiplus::REAL>(image.width * pageView.scale);
            const Gdiplus::REAL drawHeight = static_cast<Gdiplus::REAL>(image.height * pageView.scale);
            const float opacity = IsImagePendingErase(pageView.pageIndex, imageIndex) ? kPendingEraseObjectOpacity : 1.0f;

            imageGraphics.ResetTransform();
            imageGraphics.TranslateTransform(centerX, centerY);
            imageGraphics.RotateTransform(image.rotationDegrees);
            if (opacity < 1.0f) {
                Gdiplus::ColorMatrix matrix {
                    1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, opacity, 0.0f,
                    0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                };
                Gdiplus::ImageAttributes attributes;
                attributes.SetColorMatrix(&matrix);
                Gdiplus::RectF destRect(-drawWidth * 0.5f, -drawHeight * 0.5f, drawWidth, drawHeight);
                imageGraphics.DrawImage(
                    &bitmap,
                    destRect,
                    0.0f,
                    0.0f,
                    static_cast<Gdiplus::REAL>(image.pixelWidth),
                    static_cast<Gdiplus::REAL>(image.pixelHeight),
                    Gdiplus::UnitPixel,
                    &attributes);
            } else {
                imageGraphics.DrawImage(&bitmap, -drawWidth * 0.5f, -drawHeight * 0.5f, drawWidth, drawHeight);
            }
        }
        imageGraphics.ResetTransform();

        for (std::size_t textIndex = 0; textIndex < page.texts.size(); ++textIndex) {
            if (IsEditingText(pageView.pageIndex, textIndex)) {
                continue;
            }
            const float opacity = IsTextPendingErase(pageView.pageIndex, textIndex) ? kPendingEraseObjectOpacity : 1.0f;
            PaintText(hdc, page.texts[textIndex], pageView, opacity);
        }

        for (std::size_t strokeIndex = 0; strokeIndex < page.strokes.size(); ++strokeIndex) {
            const float opacity = IsStrokePendingErase(pageView.pageIndex, strokeIndex) ? kPendingEraseObjectOpacity : 1.0f;
            PaintStroke(hdc, page.strokes[strokeIndex], pageView, false, false, opacity);
        }
    } else if (interactionSnapshot != nullptr) {
        BITMAPINFO bitmapInfo {};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = interactionSnapshot->width;
        bitmapInfo.bmiHeader.biHeight = -interactionSnapshot->height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchDIBits(
            hdc,
            pageView.rect.left,
            pageView.rect.top,
            pageView.rect.right - pageView.rect.left,
            pageView.rect.bottom - pageView.rect.top,
            0,
            0,
            interactionSnapshot->width,
            interactionSnapshot->height,
            interactionSnapshot->pixels.data(),
            &bitmapInfo,
            DIB_RGB_COLORS,
            SRCCOPY);
    } else if (zoomSnapshot != nullptr) {
        BITMAPINFO bitmapInfo {};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = zoomSnapshot->width;
        bitmapInfo.bmiHeader.biHeight = -zoomSnapshot->height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchDIBits(
            hdc,
            pageView.rect.left,
            pageView.rect.top,
            pageView.rect.right - pageView.rect.left,
            pageView.rect.bottom - pageView.rect.top,
            0,
            0,
            zoomSnapshot->width,
            zoomSnapshot->height,
            zoomSnapshot->pixels.data(),
            &bitmapInfo,
            DIB_RGB_COLORS,
            SRCCOPY);
    } else if (pageView.pageIndex < pageSurfaceCaches_.size()) {
        const PageSurfaceCache& cache = pageSurfaceCaches_[pageView.pageIndex];
        if (cache.valid && !cache.pixels.empty()) {
            BITMAPINFO bitmapInfo {};
            bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
            bitmapInfo.bmiHeader.biWidth = cache.width;
            bitmapInfo.bmiHeader.biHeight = -cache.height;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;

            const int stretchMode = (isPanning_ || smoothScrollActive_ || zoomSessionActive_ || interactionPreviewMode_ != InteractionPreviewMode::None)
                ? COLORONCOLOR
                : HALFTONE;
            SetStretchBltMode(hdc, stretchMode);
            if (stretchMode == HALFTONE) {
                SetBrushOrgEx(hdc, 0, 0, nullptr);
            }
            StretchDIBits(
                hdc,
                pageView.rect.left,
                pageView.rect.top,
                pageView.rect.right - pageView.rect.left,
                pageView.rect.bottom - pageView.rect.top,
                0,
                0,
                cache.width,
                cache.height,
                cache.pixels.data(),
                &bitmapInfo,
                DIB_RGB_COLORS,
                SRCCOPY);
        }
    }

    const bool pageHasInteractionPreview = interactionPreviewMode_ != InteractionPreviewMode::None
        && ((pageView.pageIndex < interactionPreviewStrokes_.size() && !interactionPreviewStrokes_[pageView.pageIndex].empty())
            || (pageView.pageIndex < interactionPreviewTexts_.size() && !interactionPreviewTexts_[pageView.pageIndex].empty())
            || (pageView.pageIndex < interactionPreviewImages_.size() && !interactionPreviewImages_[pageView.pageIndex].empty())
            || interactionSnapshot != nullptr);
    const UINT dpi = window_ != nullptr ? WindowDpi(window_) : SystemDpi();
    bool showSelectToolSelectionAffordances = false;
    if (activeTool_ == Tool::Select) {
        showSelectToolSelectionAffordances = HasSelection();
    }
    const auto singleImage = SingleSelectedImage();
    const auto singleText = SingleSelectedText();
    const auto singleStrokePage = SingleSelectedStrokePage();
    for (std::size_t imageIndex = 0; !pageHasInteractionPreview && imageIndex < page.images.size(); ++imageIndex) {
        if (!IsImageSelected(pageView.pageIndex, imageIndex)) {
            continue;
        }

        const PageImage& image = page.images[imageIndex];
        const auto corners = ImageCorners(image);
        POINT polygon[5] {};
        for (std::size_t index = 0; index < corners.size(); ++index) {
            const Vec2 screenPoint = ScreenPointFromPagePoint(corners[index], pageView.rect, pageView.scale);
            polygon[index].x = static_cast<LONG>(std::lround(screenPoint.x));
            polygon[index].y = static_cast<LONG>(std::lround(screenPoint.y));
        }
        polygon[4] = polygon[0];
        DrawPolylineAA(hdc, polygon, 5, RGB(110, 204, 255), 2.0f, Gdiplus::DashStyleDash);

        if (singleImage.has_value() && singleImage->first == pageView.pageIndex && singleImage->second == imageIndex
            && (activeTool_ == Tool::Move || isMovingSelection_ || isTransformingImage_
                || (activeTool_ == Tool::Select && showSelectToolSelectionAffordances))) {
            const auto paintHandle = [&](int centerX, int centerY) {
                PaintSelectionHandle(hdc, centerX, centerY, dpi);
            };

            const Vec2 topMid {
                (corners[0].x + corners[1].x) * 0.5f,
                (corners[0].y + corners[1].y) * 0.5f,
            };
            const Vec2 bottomMid {
                (corners[2].x + corners[3].x) * 0.5f,
                (corners[2].y + corners[3].y) * 0.5f,
            };
            const Vec2 leftMid {
                (corners[0].x + corners[3].x) * 0.5f,
                (corners[0].y + corners[3].y) * 0.5f,
            };
            const Vec2 rightMid {
                (corners[1].x + corners[2].x) * 0.5f,
                (corners[1].y + corners[2].y) * 0.5f,
            };
            const Vec2 center = ImageCenter(image);
            const Vec2 rotatePagePoint = RotatePointAround({ center.x, image.y - SelectionRotateOffsetPageUnits(pageView.scale, dpi) }, center, image.rotationDegrees);
            const auto toScreenPoint = [&](Vec2 point) {
                return ScreenPointFromPagePoint(point, pageView.rect, pageView.scale);
            };
            const Vec2 nw = toScreenPoint(corners[0]);
            const Vec2 n = toScreenPoint(topMid);
            const Vec2 ne = toScreenPoint(corners[1]);
            const Vec2 e = toScreenPoint(rightMid);
            const Vec2 se = toScreenPoint(corners[2]);
            const Vec2 s = toScreenPoint(bottomMid);
            const Vec2 sw = toScreenPoint(corners[3]);
            const Vec2 w = toScreenPoint(leftMid);
            const Vec2 rotateScreenPoint = toScreenPoint(rotatePagePoint);

            paintHandle(static_cast<int>(std::lround(nw.x)), static_cast<int>(std::lround(nw.y)));
            paintHandle(static_cast<int>(std::lround(n.x)), static_cast<int>(std::lround(n.y)));
            paintHandle(static_cast<int>(std::lround(ne.x)), static_cast<int>(std::lround(ne.y)));
            paintHandle(static_cast<int>(std::lround(e.x)), static_cast<int>(std::lround(e.y)));
            paintHandle(static_cast<int>(std::lround(se.x)), static_cast<int>(std::lround(se.y)));
            paintHandle(static_cast<int>(std::lround(s.x)), static_cast<int>(std::lround(s.y)));
            paintHandle(static_cast<int>(std::lround(sw.x)), static_cast<int>(std::lround(sw.y)));
            paintHandle(static_cast<int>(std::lround(w.x)), static_cast<int>(std::lround(w.y)));

            HPEN stemPen = CreatePen(PS_SOLID, std::max(1, ScaleForDpi(1, dpi)), RGB(110, 204, 255));
            HGDIOBJ oldStemPen = SelectObject(hdc, stemPen);
            MoveToEx(hdc, static_cast<int>(std::lround(n.x)), static_cast<int>(std::lround(n.y)), nullptr);
            LineTo(hdc, static_cast<int>(std::lround(rotateScreenPoint.x)), static_cast<int>(std::lround(rotateScreenPoint.y)));
            SelectObject(hdc, oldStemPen);
            DeleteObject(stemPen);
            paintHandle(static_cast<int>(std::lround(rotateScreenPoint.x)), static_cast<int>(std::lround(rotateScreenPoint.y)));
        }
    }

    for (std::size_t textIndex = 0; !pageHasInteractionPreview && textIndex < page.texts.size(); ++textIndex) {
        if (!IsTextSelected(pageView.pageIndex, textIndex)) {
            continue;
        }
        RECT screenRect {};
        bool hasScreenRect = false;
        PageText overlayText = page.texts[textIndex];
        if (IsEditingText(pageView.pageIndex, textIndex)) {
            RECT liveEditRect {};
            if (textEditWindow_ != nullptr && IsWindowVisible(textEditWindow_) != FALSE
                && GetWindowRect(textEditWindow_, &liveEditRect) != FALSE) {
                MapWindowPoints(HWND_DESKTOP, window_, reinterpret_cast<POINT*>(&liveEditRect), 2);
                screenRect = liveEditRect;
                hasScreenRect = HasRect(screenRect);
            } else if (HasRect(textEditBounds_)) {
                screenRect = textEditBounds_;
                hasScreenRect = true;
            } else if (const auto liveText = CurrentEditingTextLayout(); liveText.has_value()) {
                overlayText = *liveText;
            }
        }

        if (!hasScreenRect) {
            if (const auto textRect = PageTextBoundsRect(overlayText, page.width, pageView.scale, 4); textRect.has_value()) {
                screenRect = *textRect;
                OffsetRect(&screenRect, pageView.rect.left, pageView.rect.top);
                hasScreenRect = true;
            }
        }

        if (hasScreenRect) {
            if (!IsEditingText(pageView.pageIndex, textIndex)) {
                DrawRectangleAA(hdc, screenRect, RGB(110, 204, 255), 2.0f, Gdiplus::DashStyleDash);
            }

            if (singleText.has_value() && singleText->first == pageView.pageIndex && singleText->second == textIndex
                && !IsEditingText(pageView.pageIndex, textIndex)
                && (activeTool_ == Tool::Move || isTransformingText_
                    || (activeTool_ == Tool::Select && showSelectToolSelectionAffordances))) {
                const auto paintHandle = [&](int centerX, int centerY) {
                    PaintSelectionHandle(hdc, centerX, centerY, dpi);
                };

                const int left = screenRect.left;
                const int top = screenRect.top;
                const int right = screenRect.right;
                const int bottom = screenRect.bottom;
                const int centerX = (left + right) / 2;
                const int centerY = (top + bottom) / 2;
                paintHandle(left, top);
                paintHandle(centerX, top);
                paintHandle(right, top);
                paintHandle(right, centerY);
                paintHandle(right, bottom);
                paintHandle(centerX, bottom);
                paintHandle(left, bottom);
                paintHandle(left, centerY);
            }
        }
    }

    for (std::size_t strokeIndex = 0; !pageHasInteractionPreview && strokeIndex < page.strokes.size(); ++strokeIndex) {
        if (!IsStrokeSelected(pageView.pageIndex, strokeIndex)) {
            continue;
        }

        Stroke highlight = page.strokes[strokeIndex];
        highlight.style.color = 0xB06ECFFF;
        highlight.style.width += 4.0f;
        PaintStroke(hdc, highlight, pageView, false);
    }

    if (!pageHasInteractionPreview && singleStrokePage.has_value() && *singleStrokePage == pageView.pageIndex
        && pageView.pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageView.pageIndex].empty()
        && (activeTool_ == Tool::Select || activeTool_ == Tool::Move || isMovingSelection_ || isTransformingStroke_)) {
        float minX = page.width;
        float minY = page.height;
        float maxX = 0.0f;
        float maxY = 0.0f;
        for (std::size_t strokeIndex : selectedStrokeIndices_[pageView.pageIndex]) {
            if (strokeIndex >= page.strokes.size()) {
                continue;
            }
            for (const Vec2& point : page.strokes[strokeIndex].points) {
                minX = std::min(minX, point.x);
                minY = std::min(minY, point.y);
                maxX = std::max(maxX, point.x);
                maxY = std::max(maxY, point.y);
            }
        }

        const int left = pageView.rect.left + static_cast<int>(std::lround(minX * pageView.scale));
        const int top = pageView.rect.top + static_cast<int>(std::lround(minY * pageView.scale));
        const int right = pageView.rect.left + static_cast<int>(std::lround(maxX * pageView.scale));
        const int bottom = pageView.rect.top + static_cast<int>(std::lround(maxY * pageView.scale));
        DrawRectangleAA(hdc, MakeRect(left, top, right, bottom), RGB(110, 204, 255), 2.0f, Gdiplus::DashStyleDash);

        const bool shouldDrawHandles = activeTool_ == Tool::Move || isMovingSelection_ || isTransformingStroke_
            || (activeTool_ == Tool::Select && showSelectToolSelectionAffordances);
        if (shouldDrawHandles) {
            const auto paintHandle = [&](int centerX, int centerY) { PaintSelectionHandle(hdc, centerX, centerY, dpi); };

            const int centerX = (left + right) / 2;
            const int centerY = (top + bottom) / 2;
            paintHandle(left, top);
            paintHandle(centerX, top);
            paintHandle(right, top);
            paintHandle(right, centerY);
            paintHandle(right, bottom);
            paintHandle(centerX, bottom);
            paintHandle(left, bottom);
            paintHandle(left, centerY);

            const int rotateY = top - ScaleForDpi(static_cast<int>(kSelectionRotateOffsetDip), dpi);
            HPEN stemPen = CreatePen(PS_SOLID, std::max(1, ScaleForDpi(1, dpi)), RGB(110, 204, 255));
            HGDIOBJ oldStemPen = SelectObject(hdc, stemPen);
            MoveToEx(hdc, centerX, top, nullptr);
            LineTo(hdc, centerX, rotateY);
            SelectObject(hdc, oldStemPen);
            DeleteObject(stemPen);
            paintHandle(centerX, rotateY);
        }
    }

    if (activeStroke_.has_value() && activeStrokePageIndex_.has_value() && *activeStrokePageIndex_ == pageView.pageIndex) {
        PaintStroke(hdc, *activeStroke_, pageView, true);
    }

    if (pageHasInteractionPreview) {
        PaintInteractionPreview(hdc, pageView, showSelectToolSelectionAffordances);
    }

    RestoreDC(hdc, -1);
}

void App::PaintStroke(HDC hdc, const Stroke& stroke, const PageView& pageView, bool, bool fastPreview, float opacity) const {
    if (stroke.points.empty()) {
        return;
    }

    const float strokeSmoothing = StrokeRenderSmoothing(stroke, settings_.strokeSmoothing);
    const std::vector<Vec2> smoothedPoints = BuildSmoothedPoints(stroke.points, strokeSmoothing);
    const float penWidth = std::max(1.25f, stroke.style.width * pageView.scale / 2.15f);

    if (gdiplusToken_ != 0) {
        std::vector<Gdiplus::PointF> screenPoints;
        screenPoints.reserve(smoothedPoints.size());
        for (const Vec2& point : smoothedPoints) {
            screenPoints.emplace_back(
                static_cast<Gdiplus::REAL>(pageView.rect.left + (point.x * pageView.scale)),
                static_cast<Gdiplus::REAL>(pageView.rect.top + (point.y * pageView.scale)));
        }

        const float clampedOpacity = std::clamp(opacity, 0.0f, 1.0f);
        const auto alpha = static_cast<BYTE>(std::lround(static_cast<float>((stroke.style.color >> 24U) & 0xFFU) * clampedOpacity));
        const auto red = static_cast<BYTE>((stroke.style.color >> 16U) & 0xFFU);
        const auto green = static_cast<BYTE>((stroke.style.color >> 8U) & 0xFFU);
        const auto blue = static_cast<BYTE>(stroke.style.color & 0xFFU);

        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(fastPreview ? Gdiplus::SmoothingModeHighSpeed : Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(fastPreview ? Gdiplus::PixelOffsetModeDefault : Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(fastPreview ? Gdiplus::CompositingQualityHighSpeed : Gdiplus::CompositingQualityHighQuality);

        Gdiplus::Color color(alpha, red, green, blue);
        Gdiplus::Pen pen(color, penWidth);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        if (stroke.style.dotted) {
            const Gdiplus::REAL dashPattern[] {
                static_cast<Gdiplus::REAL>(std::max(0.5f, settings_.dashLength)),
                static_cast<Gdiplus::REAL>(std::max(0.5f, settings_.dashGap)),
            };
            pen.SetDashPattern(dashPattern, 2);
            pen.SetDashCap(Gdiplus::DashCapRound);
        }

        if (screenPoints.size() == 1U) {
            Gdiplus::SolidBrush brush(color);
            const Gdiplus::REAL radius = penWidth * 0.5f;
            graphics.FillEllipse(
                &brush,
                screenPoints.front().X - radius,
                screenPoints.front().Y - radius,
                penWidth,
                penWidth);
        } else if (screenPoints.size() == 2U) {
            graphics.DrawLines(&pen, screenPoints.data(), static_cast<INT>(screenPoints.size()));
        } else {
            Gdiplus::GraphicsPath path;
            path.AddCurve(screenPoints.data(), static_cast<INT>(screenPoints.size()), 0.1f + (strokeSmoothing * 0.8f));
            graphics.DrawPath(&pen, &path);
        }

        return;
    }

    const int fallbackWidth = std::max(1, static_cast<int>(std::lround(penWidth)));
    const auto blendChannel = [](BYTE channel, float opacityValue) {
        return static_cast<BYTE>(std::lround((static_cast<float>(channel) * opacityValue) + (255.0f * (1.0f - opacityValue))));
    };
    const float clampedOpacity = std::clamp(opacity, 0.0f, 1.0f);
    const BYTE red = static_cast<BYTE>((stroke.style.color >> 16U) & 0xFFU);
    const BYTE green = static_cast<BYTE>((stroke.style.color >> 8U) & 0xFFU);
    const BYTE blue = static_cast<BYTE>(stroke.style.color & 0xFFU);
    const COLORREF fallbackColor = RGB(blendChannel(red, clampedOpacity), blendChannel(green, clampedOpacity), blendChannel(blue, clampedOpacity));
    HPEN pen = CreatePen(stroke.style.dotted ? PS_DASH : PS_SOLID, fallbackWidth, fallbackColor);
    HGDIOBJ oldPen = SelectObject(hdc, pen);

    POINT first {
        pageView.rect.left + static_cast<LONG>(std::lround(smoothedPoints.front().x * pageView.scale)),
        pageView.rect.top + static_cast<LONG>(std::lround(smoothedPoints.front().y * pageView.scale)),
    };
    MoveToEx(hdc, first.x, first.y, nullptr);

    if (smoothedPoints.size() == 1U) {
        LineTo(hdc, first.x + 1, first.y + 1);
    } else {
        for (std::size_t index = 1; index < smoothedPoints.size(); ++index) {
            const POINT next {
                pageView.rect.left + static_cast<LONG>(std::lround(smoothedPoints[index].x * pageView.scale)),
                pageView.rect.top + static_cast<LONG>(std::lround(smoothedPoints[index].y * pageView.scale)),
            };
            LineTo(hdc, next.x, next.y);
        }
    }

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void App::PaintText(HDC hdc, const PageText& text, const PageView& pageView, float opacity) const {
    if (pageView.pageIndex >= document_.PageCount()) {
        return;
    }

    const Page& page = document_.PageAt(pageView.pageIndex);
    const WrappedTextLayout layout = LayoutPageText(text, page.width);
    const int fontHeight = PageTextFontPixelHeight(text, pageView.scale);
    HFONT font = CreatePageTextFont(text, fontHeight);
    HGDIOBJ previousFont = font != nullptr ? SelectObject(hdc, font) : nullptr;
    int renderLineHeight = fontHeight;
    TEXTMETRICW textMetrics {};
    if (GetTextMetricsW(hdc, &textMetrics) != 0) {
        renderLineHeight = std::max(renderLineHeight, static_cast<int>(textMetrics.tmHeight));
    }
    const int left = pageView.rect.left + static_cast<int>(std::lround(text.x * pageView.scale));
    const int top = pageView.rect.top + static_cast<int>(std::lround(text.y * pageView.scale));
    const int renderWidth = std::max(1, static_cast<int>(std::ceil(ResolvedPageTextWidth(text, page.width) * std::max(0.1f, pageView.scale))));
    const auto blendChannel = [](BYTE channel, float opacityValue) {
        return static_cast<BYTE>(std::lround((static_cast<float>(channel) * opacityValue) + (255.0f * (1.0f - opacityValue))));
    };
    const float clampedOpacity = std::clamp(opacity, 0.0f, 1.0f);
    const BYTE red = static_cast<BYTE>((text.color >> 16U) & 0xFFU);
    const BYTE green = static_cast<BYTE>((text.color >> 8U) & 0xFFU);
    const BYTE blue = static_cast<BYTE>(text.color & 0xFFU);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(blendChannel(red, clampedOpacity), blendChannel(green, clampedOpacity), blendChannel(blue, clampedOpacity)));
    const UINT alignmentFlags = DrawTextAlignmentFlags(text.alignment);
    for (std::size_t lineIndex = 0; lineIndex < layout.lines.size(); ++lineIndex) {
        const int lineTop = top + (renderLineHeight * static_cast<int>(lineIndex));
        const std::wstring& line = layout.lines[lineIndex];
        RECT lineRect = MakeRect(left, lineTop, left + renderWidth, lineTop + renderLineHeight);
        DrawTextW(
            hdc,
            line.c_str(),
            static_cast<int>(line.size()),
            &lineRect,
            alignmentFlags | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    }
    if (previousFont != nullptr) {
        SelectObject(hdc, previousFont);
    }
    if (font != nullptr) {
        DeleteObject(font);
    }
}


}  // namespace cnote
