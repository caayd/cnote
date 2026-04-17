#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include "App.h"

namespace cnote::appui {

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
};

int ToolbarHeight(bool maximized, UINT dpi);
int ToolbarTopInset(bool maximized, UINT dpi);
SidebarMetrics BuildSidebarMetrics(UINT dpi);
ToolbarTitleLayout ResolveToolbarTitleLayout(const ToolbarLayout& layout, HDC hdc, const std::wstring& title, const std::wstring& zoomLabel, UINT dpi);
ToolbarLayout BuildToolbarLayout(RECT clientRect, std::size_t colorCount, bool maximized, App::Tool activeTool, UINT dpi);
SidebarHeaderLayout BuildSidebarHeaderLayout(RECT sidebarRect, UINT dpi);
QuickSizePopupLayout BuildQuickSizePopupLayout(RECT clientRect, POINT anchor, UINT dpi, int previewExtent);
RECT QuickSizePopupBounds(const QuickSizePopupLayout& layout, UINT dpi);
TextFormatPopupLayout BuildTextFormatPopupLayout(RECT canvasRect, RECT editRect, bool fontMenuOpen, UINT dpi);
RECT TextFormatPopupBounds(const TextFormatPopupLayout& layout, UINT dpi);
PaletteMenuLayout BuildPaletteMenuLayout(const ToolbarLayout& toolbarLayout, std::size_t colorCount, UINT dpi);
PaletteWheelLayout BuildPaletteWheelLayout(RECT clientRect, POINT requestedCenter, std::size_t colorCount, int toolbarHeight, UINT dpi);
std::optional<std::size_t> PaletteWheelIndexAtPoint(const PaletteWheelLayout& layout, POINT cursor, std::size_t colorCount);
bool IsToolbarControlRect(const ToolbarLayout& layout, const ToolbarTitleLayout& titleLayout, POINT cursor);
RECT ToolbarBrushTrackRect(const RECT& sliderRect, UINT dpi);
int ToolbarBrushThumbCenterX(const RECT& sliderRect, float brushSize);

}  // namespace cnote::appui
