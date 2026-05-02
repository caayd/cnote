#pragma once

#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include "Document.h"

namespace cnote::text {

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

UINT DrawTextAlignmentFlags(TextAlignment alignment);
const wchar_t* PageTextFontFace(TextFontFamily family);
int PageTextFontWeight(const PageText& text);
HFONT CreatePageTextFont(const PageText& text, int pixelHeight, DWORD quality = CLEARTYPE_QUALITY);
int PageTextFontPixelHeight(const PageText& text, float scale);
float DefaultPageTextBoxWidth(const Page& page, const PageText& text);
float ResolvedPageTextWidth(const PageText& text, float pageWidth);
WrappedTextLayout LayoutPageText(const PageText& text, float pageWidth);
TextPixelMetrics MeasurePageTextPixels(const PageText& text, float pageWidth, float scale);
PageTextBounds MeasurePageTextBounds(const PageText& text, float pageWidth);
std::optional<RECT> PageTextBoundsRect(const PageText& text, float pageWidth, float scale, int extraPadding = 0);
std::optional<RECT> PageTextCachedBoundsRect(const PageText& text, float boundsWidth, float boundsHeight, float scale, int extraPadding = 0);
bool TextHitsPoint(const PageText& text, float pageWidth, Vec2 point);
bool TextIntersectsRectSelection(const PageText& text, float pageWidth, Vec2 anchor, Vec2 current);
bool TextIntersectsPolygonSelection(const PageText& text, float pageWidth, const std::vector<Vec2>& polygon);
std::wstring TrimTrailingLayoutWhitespace(std::wstring value);

}  // namespace cnote::text
