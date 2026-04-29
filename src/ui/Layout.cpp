#include "ui/Layout.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "DpiUtil.h"

namespace cnote::appui {

namespace {

constexpr int kIconButtonSize = 30;
constexpr int kColorSwatchSize = 22;
constexpr int kBaseToolbarHeight = 34;
constexpr int kMaximizedToolbarExtraHeight = 5;
constexpr int kTitleBarButtonHeight = 34;
constexpr int kPaletteWheelShadowInsetDip = 6;
constexpr int kPaletteWheelHoverGrowDip = 8;
constexpr float kPaletteWheelSelectedStrokeDip = 3.0f;

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect { left, top, right, bottom };
    return rect;
}

bool HasRect(const RECT& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool RectsOverlap(const RECT& a, const RECT& b) {
    RECT overlap {};
    return IntersectRect(&overlap, &a, &b) != 0;
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

float SnapBrushSize(float size) {
    constexpr float kMinBrushSize = 0.5f;
    constexpr float kMaxBrushSize = 20.0f;
    constexpr float kBrushSizeStep = 0.25f;
    const float clamped = std::clamp(size, kMinBrushSize, kMaxBrushSize);
    const float steps = std::round((clamped - kMinBrushSize) / kBrushSizeStep);
    return kMinBrushSize + (steps * kBrushSizeStep);
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

}  // namespace

int ToolbarHeight(bool maximized, UINT dpi) {
    return ScaleForDpi(kBaseToolbarHeight + (maximized ? kMaximizedToolbarExtraHeight : 0), dpi);
}

int ToolbarTopInset(bool maximized, UINT dpi) {
    return maximized ? ScaleForDpi(kMaximizedToolbarExtraHeight, dpi) : 0;
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

ToolbarTitleLayout ResolveToolbarTitleLayout(const ToolbarLayout& layout, HDC hdc, const std::wstring& title, const std::wstring& zoomLabel, UINT dpi) {
    ToolbarTitleLayout resolved {};
    if (!HasRect(layout.titleAreaRect)) {
        return resolved;
    }

    const int titleLeftInset = ScaleForDpi(4, dpi);
    const int titleRightInset = ScaleForDpi(4, dpi);
    const int zoomGap = ScaleForDpi(12, dpi);
    const int zoomHorizontalPadding = ScaleForDpi(6, dpi);
    const int minimumTitleWidth = ScaleForDpi(24, dpi);
    const int minimumZoomWidth = ScaleForDpi(44, dpi);
    const int maximumZoomWidth = ScaleForDpi(72, dpi);

    const int availableWidth = static_cast<int>(layout.titleAreaRect.right - layout.titleAreaRect.left);
    const int zoomWidth = std::clamp(
        MeasureToolbarTextWidth(hdc, zoomLabel) + (zoomHorizontalPadding * 2),
        minimumZoomWidth,
        maximumZoomWidth);
    const int maxTitleWidth = std::max(minimumTitleWidth, availableWidth - zoomGap - zoomWidth);
    const int titleWidth = std::clamp(
        MeasureToolbarTextWidth(hdc, title) + titleLeftInset + titleRightInset,
        minimumTitleWidth,
        maxTitleWidth);

    resolved.fileTitleRect = MakeRect(
        layout.titleAreaRect.left,
        layout.titleAreaRect.top,
        layout.titleAreaRect.left + titleWidth,
        layout.titleAreaRect.bottom);
    resolved.zoomRect = MakeRect(
        resolved.fileTitleRect.right + zoomGap,
        layout.titleAreaRect.top,
        resolved.fileTitleRect.right + zoomGap + zoomWidth,
        layout.titleAreaRect.bottom);
    if (!HasRect(resolved.fileTitleRect) || !HasRect(resolved.zoomRect)) {
        return {};
    }

    resolved.titleTextRect = resolved.fileTitleRect;
    resolved.titleTextRect.left += titleLeftInset;
    resolved.titleTextRect.right -= titleRightInset;

    resolved.zoomTextRect = resolved.zoomRect;
    resolved.zoomTextRect.left += zoomHorizontalPadding;
    resolved.zoomTextRect.right -= zoomHorizontalPadding;
    resolved.dividerX = resolved.fileTitleRect.right + (zoomGap / 2);
    return resolved;
}

ToolbarLayout BuildToolbarLayout(RECT clientRect, std::size_t colorCount, bool maximized, App::Tool activeTool, UINT dpi) {
    ToolbarLayout layout {};
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
    const int panelWidth = ScaleForDpi(188, dpi);
    const int panelHeight = ScaleForDpi(38, dpi);
    const int previewBounds = std::max(ScaleForDpi(28, dpi), previewExtent);
    const int cursorGap = ScaleForDpi(14, dpi);
    const int previewGap = ScaleForDpi(5, dpi);
    const int totalHeight = previewBounds + previewGap + panelHeight;
    const int toolbarBottom = clientRect.top;
    const int minLeft = static_cast<int>(clientRect.left) + ScaleForDpi(8, dpi);
    const int maxLeft = std::max(minLeft, static_cast<int>(clientRect.right) - panelWidth - ScaleForDpi(8, dpi));
    const int minTop = toolbarBottom + ScaleForDpi(8, dpi);
    const int maxTop = std::max(minTop, static_cast<int>(clientRect.bottom) - totalHeight - ScaleForDpi(8, dpi));
    int left = anchor.x - (panelWidth / 2);
    int top = anchor.y - totalHeight - cursorGap;
    left = std::clamp(left, minLeft, maxLeft);
    top = std::clamp(top, minTop, maxTop);
    const int panelTop = top + previewBounds + previewGap;
    layout.panelRect = MakeRect(left, panelTop, left + panelWidth, panelTop + panelHeight);
    layout.sliderRect = MakeRect(left + ScaleForDpi(10, dpi), panelTop + ScaleForDpi(10, dpi), left + ScaleForDpi(144, dpi), panelTop + ScaleForDpi(28, dpi));
    layout.valueRect = MakeRect(left + ScaleForDpi(150, dpi), panelTop + ScaleForDpi(6, dpi), left + ScaleForDpi(178, dpi), panelTop + ScaleForDpi(32, dpi));
    const int sliderCenterX = (layout.sliderRect.left + layout.sliderRect.right) / 2;
    layout.previewRect = MakeRect(
        sliderCenterX - (previewBounds / 2),
        top,
        sliderCenterX + (previewBounds / 2),
        top + previewBounds);
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

RECT TextFormatPopupBounds(const TextFormatPopupLayout& layout, UINT dpi) {
    RECT bounds = layout.panelRect;
    if (HasRect(layout.fontMenuRect)) {
        bounds = UnionRectangles(bounds, layout.fontMenuRect);
    }
    return InflateRectCopy(bounds, ScaleForDpi(3, dpi));
}

PaletteMenuLayout BuildPaletteMenuLayout(const ToolbarLayout& toolbarLayout, std::size_t colorCount, UINT dpi) {
    PaletteMenuLayout layout {};
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
    PaletteWheelLayout layout {};
    layout.innerRadius = ScaleForDpi(20, dpi);
    layout.outerRadius = colorCount <= 1U ? ScaleForDpi(40, dpi) : ScaleForDpi(86, dpi);
    layout.hitOuterRadius = layout.outerRadius + ScaleForDpi(kPaletteWheelHoverGrowDip, dpi);
    const int margin = layout.hitOuterRadius
        + ScaleForDpi(kPaletteWheelShadowInsetDip, dpi)
        + static_cast<int>(std::ceil(std::max(1.0f, ScaleForDpi(kPaletteWheelSelectedStrokeDip, dpi))))
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

    constexpr float wheelPi = 3.14159265358979323846f;
    float angle = std::atan2(dy, dx);
    angle += wheelPi / 2.0f;
    if (angle < 0.0f) {
        angle += 2.0f * wheelPi;
    }

    const float normalized = angle / (2.0f * wheelPi);
    const std::size_t index = std::min(
        colorCount - 1U,
        static_cast<std::size_t>(std::floor(normalized * static_cast<float>(colorCount))));
    return index;
}

bool IsToolbarControlRect(const ToolbarLayout& layout, const ToolbarTitleLayout& titleLayout, POINT cursor) {
    const std::array<RECT, 12> controls {
        titleLayout.fileTitleRect,
        titleLayout.zoomRect,
        layout.minimizeRect,
        layout.maximizeRect,
        layout.closeRect,
        layout.undoRect,
        layout.redoRect,
        layout.selectToolRect,
        layout.textToolRect,
        layout.moveToolRect,
        layout.drawToolRect,
        layout.panToolRect,
    };

    for (const RECT& rect : controls) {
        if (PtInRect(&rect, cursor) != 0) {
            return true;
        }
    }

    if (PtInRect(&layout.eraseToolRect, cursor) != 0) {
        return true;
    }

    return PtInRect(&layout.currentColorRect, cursor) != 0;
}

RECT ToolbarBrushTrackRect(const RECT& sliderRect, UINT dpi) {
    const int centerY = (sliderRect.top + sliderRect.bottom) / 2;
    return MakeRect(
        sliderRect.left + ScaleForDpi(10, dpi),
        centerY - ScaleForDpi(3, dpi),
        sliderRect.right - ScaleForDpi(10, dpi),
        centerY + ScaleForDpi(3, dpi));
}

int ToolbarBrushThumbCenterX(const RECT& sliderRect, float brushSize, float maxBrushSize) {
    const int width = std::max(1, static_cast<int>(sliderRect.right - sliderRect.left));
    const float sliderMax = std::max(1.0f, maxBrushSize);
    const float span = std::max(0.001f, sliderMax - 0.5f);
    const float normalized = std::clamp((std::min(SnapBrushSize(brushSize), sliderMax) - 0.5f) / span, 0.0f, 1.0f);
    return sliderRect.left + static_cast<int>(std::lround(normalized * static_cast<float>(width - 1)));
}

}  // namespace cnote::appui
