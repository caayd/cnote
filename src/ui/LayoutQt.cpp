#include "ui/LayoutQt.h"

#include <algorithm>
#include <cmath>

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QString>

namespace cnote::appui::qt {

namespace {

// Reference constants.  Every value is the legacy 96-DPI logical value from the
// copy in src/ui/Layout.cpp (anonymous namespace) and the identical copy in
// src/App.cpp; the call paths in parentheses are the active reference callers.
constexpr qreal kIconButtonSize = 30.0;                     // Layout.cpp:19 (BuildToolbarLayout)
constexpr qreal kColorSwatchSize = 22.0;                    // Layout.cpp:20 (BuildToolbarLayout)
constexpr qreal kBaseToolbarHeight = 34.0;                  // Layout.cpp:21 (ToolbarHeight)
constexpr qreal kMaximizedToolbarExtraHeight = 5.0;         // Layout.cpp:22 (ToolbarHeight)
constexpr qreal kTitleBarButtonHeight = 34.0;               // Layout.cpp:23 (BuildToolbarLayout)
constexpr qreal kPaletteWheelShadowInsetDip = 6.0;          // Layout.cpp:25 (BuildPaletteWheelLayout)
// Palette-wheel interaction slop: the appui interaction copy
// (src/ui/Layout.cpp:26) used by App::UpdatePaletteWheelHover
// (ToolbarState.cpp:202) and MessageLoop.cpp:415.  The App.cpp paint copy uses
// 12 for its hover feedback; see PaletteWheelHoverGrow() below.
constexpr qreal kPaletteWheelHoverGrowDip = 8.0;
constexpr qreal kPaletteWheelSelectedStrokeDip = 3.0;       // Layout.cpp:27 (BuildPaletteWheelLayout)
// Paint-only hover feedback from the App.cpp paint copy (App.cpp:158-159).
constexpr qreal kPaletteWheelPaintHoverGrowDip = 12.0;
constexpr qreal kPaletteWheelPaintHoverOffsetDip = 10.0;
constexpr qreal kPaletteWheelPaintIconHalfDip = 20.0;
// Toolbar auto-hide reveal bands (App.cpp:2465-2467
// App::UpdateToolbarAutoHide).
constexpr qreal kMaximizedRevealHeight = 22.0;
constexpr qreal kNormalRevealHeight = 12.0;

// The reference computes several positions in int (e.g. Layout.cpp:230,
// 234-235) and therefore truncates toward zero; reproducing that keeps the
// port within half a logical pixel of the reference on fractional results such
// as (toolbarHeight - actionHeight) / 2 in the maximized state (39 - 30).
qreal LegacyDiv(qreal numerator, qreal denominator) {
    return std::trunc(numerator / denominator);
}

QRectF MakeRect(qreal left, qreal top, qreal right, qreal bottom) {
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

bool HasRect(const QRectF& rect) {
    return rect.width() > 0.0 && rect.height() > 0.0;
}

bool RectsOverlap(const QRectF& a, const QRectF& b) {
    return a.intersects(b);
}

// Exclusive right/bottom edges, i.e. Win32 PtInRect (UiUtil/MessageLoop
// behaviour); QRectF::contains() would also accept the right/bottom edge.
bool ContainsPoint(const QRectF& rect, const QPointF& point) {
    return point.x() >= rect.left() && point.x() < rect.right()
        && point.y() >= rect.top() && point.y() < rect.bottom();
}

// Fixed 96-DPI reference device (plan step 3.6): text metrics must not pick up
// the screen's devicePixelRatio, or chrome text would be measured at the
// physical scale instead of the logical one.
QPaintDevice& ReferenceDevice() {
    static QImage device(1, 1, QImage::Format_ARGB32);
    return device;
}

qreal MeasureTextWidth(const QFont& font, const QString& text) {
    if (text.isEmpty()) {
        return 0.0;
    }
    return QFontMetricsF(font, &ReferenceDevice()).horizontalAdvance(text);
}

float SnapBrushSize(float size) {
    // App::SnapBrushSize (App.cpp:2236) -> SnapBrushSizeImpl (App.cpp:1495).
    constexpr float kMinBrushSize = 0.5f;
    constexpr float kMaxBrushSize = 20.0f;
    constexpr float kBrushSizeStep = 0.25f;
    const float clamped = std::clamp(size, kMinBrushSize, kMaxBrushSize);
    const float steps = std::round((clamped - kMinBrushSize) / kBrushSizeStep);
    return kMinBrushSize + (steps * kBrushSizeStep);
}

qreal WheelPi() {
    return 3.14159265358979323846;
}

}  // namespace

qreal ToolbarHeight(bool maximized) {
    return kBaseToolbarHeight + (maximized ? kMaximizedToolbarExtraHeight : 0.0);
}

qreal ToolbarTopInset(bool maximized) {
    return maximized ? kMaximizedToolbarExtraHeight : 0.0;
}

qreal CaptionButtonReserve() {
    return (35.0 * 3.0) + 12.0;
}

qreal PaletteWheelHoverGrow() {
    return kPaletteWheelPaintHoverGrowDip;
}

qreal PaletteWheelHoverOffset() {
    return kPaletteWheelPaintHoverOffsetDip;
}

qreal PaletteWheelIconHalfSize() {
    return kPaletteWheelPaintIconHalfDip;
}

SidebarMetrics BuildSidebarMetrics() {
    SidebarMetrics metrics;
    metrics.width = 176.0;               // Layout.cpp:77
    metrics.itemInset = 12.0;            // Layout.cpp:78
    metrics.contentPadding = 12.0;       // Layout.cpp:79
    metrics.itemGap = 10.0;              // Layout.cpp:80
    metrics.labelHeight = 22.0;          // Layout.cpp:81
    metrics.minThumbHeight = 56.0;       // Layout.cpp:82
    metrics.placeholderInset = 14.0;     // Layout.cpp:83
    metrics.placeholderLineGap = 10.0;   // Layout.cpp:84
    return metrics;
}

ToolbarTitleLayout ResolveToolbarTitleLayoutWithWidths(const ToolbarLayout& layout, qreal measuredTitleWidth, qreal measuredZoomWidth) {
    ToolbarTitleLayout resolved;
    if (!HasRect(layout.titleAreaRect)) {
        return resolved;
    }

    constexpr qreal titleLeftInset = 4.0;        // Layout.cpp:132
    constexpr qreal titleRightInset = 4.0;       // Layout.cpp:133
    constexpr qreal zoomGap = 12.0;              // Layout.cpp:134
    constexpr qreal zoomHorizontalPadding = 6.0; // Layout.cpp:135
    constexpr qreal minimumTitleWidth = 24.0;    // Layout.cpp:136
    constexpr qreal minimumZoomWidth = 44.0;     // Layout.cpp:137
    constexpr qreal maximumZoomWidth = 72.0;     // Layout.cpp:138

    const qreal availableWidth = layout.titleAreaRect.width();
    const qreal zoomWidth = std::clamp(
        measuredZoomWidth + (zoomHorizontalPadding * 2.0),
        minimumZoomWidth,
        maximumZoomWidth);
    const qreal maxTitleWidth = std::max(minimumTitleWidth, availableWidth - zoomGap - zoomWidth);
    const qreal titleWidth = std::clamp(
        measuredTitleWidth + titleLeftInset + titleRightInset,
        minimumTitleWidth,
        maxTitleWidth);

    resolved.fileTitleRect = MakeRect(
        layout.titleAreaRect.left(),
        layout.titleAreaRect.top(),
        layout.titleAreaRect.left() + titleWidth,
        layout.titleAreaRect.bottom());
    resolved.zoomRect = MakeRect(
        resolved.fileTitleRect.right() + zoomGap,
        layout.titleAreaRect.top(),
        resolved.fileTitleRect.right() + zoomGap + zoomWidth,
        layout.titleAreaRect.bottom());
    if (!HasRect(resolved.fileTitleRect) || !HasRect(resolved.zoomRect)) {
        return {};
    }

    resolved.titleTextRect = resolved.fileTitleRect;
    resolved.titleTextRect.setLeft(resolved.titleTextRect.left() + titleLeftInset);
    resolved.titleTextRect.setRight(resolved.titleTextRect.right() - titleRightInset);

    resolved.zoomTextRect = resolved.zoomRect;
    resolved.zoomTextRect.setLeft(resolved.zoomTextRect.left() + zoomHorizontalPadding);
    resolved.zoomTextRect.setRight(resolved.zoomTextRect.right() - zoomHorizontalPadding);
    resolved.dividerX = resolved.fileTitleRect.right() + LegacyDiv(zoomGap, 2.0);
    return resolved;
}

ToolbarTitleLayout ResolveToolbarTitleLayout(const ToolbarLayout& layout, const QFont& font, const QString& title, const QString& zoomLabel) {
    return ResolveToolbarTitleLayoutWithWidths(layout, MeasureChromeTextWidth(font, title), MeasureChromeTextWidth(font, zoomLabel));
}

qreal MeasureChromeTextWidth(const QFont& font, const QString& text) {
    return MeasureTextWidth(font, text);
}

ToolbarLayout BuildToolbarLayout(const QRectF& clientRect, std::size_t colorCount, bool maximized, int activeTool, qreal frameThicknessX) {
    ToolbarLayout layout;
    // colorCount and activeTool are accepted for call-path parity; the
    // reference also ignores both (Layout.cpp:206-207).
    static_cast<void>(colorCount);
    static_cast<void>(activeTool);

    const qreal toolbarHeight = ToolbarHeight(maximized);
    const qreal topInset = ToolbarTopInset(maximized);
    const qreal titleBarInset = maximized ? frameThicknessX : 0.0;
    const qreal buttonStripRight = clientRect.right() - titleBarInset;
    const qreal availableWidth = clientRect.width();
    const bool compactTitle = availableWidth < 760.0;            // Layout.cpp:212
    const qreal toolGap = compactTitle ? 8.0 : 10.0;             // Layout.cpp:213
    const qreal outerGap = compactTitle ? 12.0 : 18.0;           // Layout.cpp:214
    constexpr qreal captionButtonWidth = 35.0;                   // Layout.cpp:215

    layout.closeRect = MakeRect(buttonStripRight - captionButtonWidth, topInset, buttonStripRight, toolbarHeight);
    layout.maximizeRect = MakeRect(layout.closeRect.left() - captionButtonWidth, topInset, layout.closeRect.left(), toolbarHeight);
    layout.minimizeRect = MakeRect(layout.maximizeRect.left() - captionButtonWidth, topInset, layout.maximizeRect.left(), toolbarHeight);

    const qreal rightEdge = buttonStripRight - CaptionButtonReserve();
    const qreal toolbarTop = topInset + LegacyDiv(kTitleBarButtonHeight - kIconButtonSize, 2.0);
    const qreal colorTop = topInset + LegacyDiv(kTitleBarButtonHeight - kColorSwatchSize, 2.0);
    const qreal colorsLeft = rightEdge - kColorSwatchSize;

    layout.currentColorRect = MakeRect(colorsLeft, colorTop, colorsLeft + kColorSwatchSize, colorTop + kColorSwatchSize);

    const qreal toolBandRight = colorsLeft - outerGap;
    layout.panToolRect = MakeRect(toolBandRight - kIconButtonSize, toolbarTop, toolBandRight, toolbarTop + kIconButtonSize);
    layout.drawToolRect = MakeRect(layout.panToolRect.left() - toolGap - kIconButtonSize, toolbarTop, layout.panToolRect.left() - toolGap, toolbarTop + kIconButtonSize);
    layout.eraseToolRect = MakeRect(layout.drawToolRect.left() - toolGap - kIconButtonSize, toolbarTop, layout.drawToolRect.left() - toolGap, toolbarTop + kIconButtonSize);
    layout.textToolRect = MakeRect(layout.eraseToolRect.left() - toolGap - kIconButtonSize, toolbarTop, layout.eraseToolRect.left() - toolGap, toolbarTop + kIconButtonSize);
    layout.selectToolRect = MakeRect(layout.textToolRect.left() - toolGap - kIconButtonSize, toolbarTop, layout.textToolRect.left() - toolGap, toolbarTop + kIconButtonSize);
    layout.moveToolRect = MakeRect(layout.selectToolRect.left() - toolGap - kIconButtonSize, toolbarTop, layout.selectToolRect.left() - toolGap, toolbarTop + kIconButtonSize);

    const qreal redoRight = layout.moveToolRect.left() - (compactTitle ? 12.0 : 16.0);  // Layout.cpp:258
    const QRectF candidateRedoRect = MakeRect(redoRight - kIconButtonSize, toolbarTop, redoRight, toolbarTop + kIconButtonSize);
    const QRectF candidateUndoRect = MakeRect(candidateRedoRect.left() - toolGap - kIconButtonSize, toolbarTop, candidateRedoRect.left() - toolGap, toolbarTop + kIconButtonSize);
    constexpr qreal titleLeft = 12.0;                            // Layout.cpp:262
    const qreal minimumTitleAreaWidth = compactTitle ? 150.0 : 210.0;  // Layout.cpp:263
    if (candidateUndoRect.left() >= titleLeft + minimumTitleAreaWidth) {
        layout.redoRect = candidateRedoRect;
        layout.undoRect = candidateUndoRect;
    }

    const qreal titleRight = (HasRect(layout.undoRect) ? layout.undoRect.left() : layout.moveToolRect.left()) - 16.0;  // Layout.cpp:268
    const qreal titleAreaWidth = std::max(0.0, titleRight - titleLeft);
    if (titleAreaWidth >= minimumTitleAreaWidth) {
        layout.titleAreaRect = MakeRect(titleLeft, topInset, titleRight, toolbarHeight);
    }

    return layout;
}

SidebarHeaderLayout BuildSidebarHeaderLayout(const QRectF& sidebarRect, bool maximized) {
    SidebarHeaderLayout layout;
    constexpr qreal padding = 6.0;         // Layout.cpp:228
    constexpr qreal gap = 4.0;             // Layout.cpp:229
    constexpr qreal actionHeight = 30.0;   // Layout.cpp:230
    const qreal headerHeight = ToolbarHeight(maximized);
    layout.headerRect = MakeRect(sidebarRect.left() + padding, sidebarRect.bottom() - headerHeight, sidebarRect.right() - padding, sidebarRect.bottom());
    const qreal rowTop = layout.headerRect.top() + LegacyDiv(headerHeight - actionHeight, 2.0);
    const qreal headerWidth = layout.headerRect.width();
    const qreal pageLabelWidth = std::clamp(
        headerWidth - (52.0 * 2.0) - (gap * 2.0),
        52.0,
        64.0);
    const qreal actionWidth = std::max(1.0, LegacyDiv(headerWidth - pageLabelWidth - (gap * 2.0), 2.0));
    const qreal rowWidth = pageLabelWidth + (actionWidth * 2.0) + (gap * 2.0);
    const qreal rowLeft = layout.headerRect.left() + std::max(0.0, LegacyDiv(headerWidth - rowWidth, 2.0));
    layout.pageLabelRect = MakeRect(rowLeft, rowTop, rowLeft + pageLabelWidth, rowTop + actionHeight);
    layout.addPageRect = MakeRect(layout.pageLabelRect.right() + gap, rowTop, layout.pageLabelRect.right() + gap + actionWidth, rowTop + actionHeight);
    layout.newPageSettingsRect = MakeRect(layout.addPageRect.right() + gap, rowTop, layout.addPageRect.right() + gap + actionWidth, rowTop + actionHeight);
    layout.contentRect = MakeRect(sidebarRect.left(), sidebarRect.top(), sidebarRect.right(), layout.headerRect.top());
    return layout;
}

QuickSizePopupLayout BuildQuickSizePopupLayout(const QRectF& clientRect, const QPointF& anchor, qreal previewExtent) {
    QuickSizePopupLayout layout;
    constexpr qreal panelWidth = 188.0;       // Layout.cpp:259
    constexpr qreal panelHeight = 38.0;       // Layout.cpp:260
    const qreal previewBounds = std::max(28.0, previewExtent);  // Layout.cpp:261
    constexpr qreal cursorGap = 14.0;         // Layout.cpp:262
    constexpr qreal previewGap = 5.0;         // Layout.cpp:263
    const qreal totalHeight = previewBounds + previewGap + panelHeight;
    const qreal toolbarBottom = clientRect.top();
    constexpr qreal edgePadding = 8.0;        // Layout.cpp:266,268
    const qreal minLeft = clientRect.left() + edgePadding;
    const qreal maxLeft = std::max(minLeft, clientRect.right() - panelWidth - edgePadding);
    const qreal minTop = toolbarBottom + edgePadding;
    const qreal maxTop = std::max(minTop, clientRect.bottom() - totalHeight - edgePadding);
    qreal left = anchor.x() - LegacyDiv(panelWidth, 2.0);
    qreal top = anchor.y() - totalHeight - cursorGap;
    left = std::clamp(left, minLeft, maxLeft);
    top = std::clamp(top, minTop, maxTop);
    const qreal panelTop = top + previewBounds + previewGap;
    layout.panelRect = MakeRect(left, panelTop, left + panelWidth, panelTop + panelHeight);
    layout.sliderRect = MakeRect(left + 10.0, panelTop + 10.0, left + 144.0, panelTop + 28.0);   // Layout.cpp:275-278
    layout.valueRect = MakeRect(left + 150.0, panelTop + 6.0, left + 178.0, panelTop + 32.0);    // Layout.cpp:279-282
    const qreal sliderCenterX = LegacyDiv(layout.sliderRect.left() + layout.sliderRect.right(), 2.0);
    layout.previewRect = MakeRect(
        sliderCenterX - LegacyDiv(previewBounds, 2.0),
        top,
        sliderCenterX + LegacyDiv(previewBounds, 2.0),
        top + previewBounds);
    return layout;
}

QRectF QuickSizePopupBounds(const QuickSizePopupLayout& layout) {
    return layout.panelRect.united(layout.previewRect).adjusted(-3.0, -3.0, 3.0, 3.0);  // Layout.cpp:288
}

TextFormatPopupLayout BuildTextFormatPopupLayout(const QRectF& canvasRect, const QRectF& editRect, bool fontMenuOpen) {
    TextFormatPopupLayout layout;
    constexpr qreal panelPadding = 8.0;        // Layout.cpp:327
    constexpr qreal buttonSize = 28.0;         // Layout.cpp:328
    constexpr qreal textSizeInputWidth = 58.0; // Layout.cpp:329
    constexpr qreal itemGap = 6.0;             // Layout.cpp:330
    constexpr qreal dropdownWidth = 118.0;     // Layout.cpp:331
    constexpr qreal panelHeight = 44.0;        // Layout.cpp:332
    const qreal panelWidth = (panelPadding * 2.0) + (buttonSize * 4.0) + textSizeInputWidth + (itemGap * 5.0) + dropdownWidth;
    constexpr qreal panelGap = 10.0;           // Layout.cpp:334
    constexpr qreal canvasPadding = 8.0;       // Layout.cpp:335
    qreal left = editRect.left();
    const qreal minLeft = canvasRect.left() + canvasPadding;
    const qreal maxLeft = std::max(minLeft, canvasRect.right() - panelWidth - canvasPadding);
    left = std::clamp(left, minLeft, maxLeft);

    qreal top = editRect.top() - panelHeight - panelGap;
    if (top < canvasRect.top() + canvasPadding) {
        top = editRect.bottom() + panelGap;
    }
    const qreal minTop = canvasRect.top() + canvasPadding;
    const qreal maxTop = std::max(minTop, canvasRect.bottom() - panelHeight - canvasPadding);
    top = std::clamp(top, minTop, maxTop);

    layout.panelRect = MakeRect(left, top, left + panelWidth, top + panelHeight);
    qreal itemLeft = layout.panelRect.left() + panelPadding;
    const qreal itemTop = layout.panelRect.top() + LegacyDiv(panelHeight - buttonSize, 2.0);
    const auto advanceButton = [&](QRectF& rect) {
        rect = MakeRect(itemLeft, itemTop, itemLeft + buttonSize, itemTop + buttonSize);
        itemLeft = rect.right() + itemGap;
    };

    advanceButton(layout.alignCycleRect);
    advanceButton(layout.boldRect);
    advanceButton(layout.italicRect);
    advanceButton(layout.underlineRect);
    layout.textSizeInputRect = MakeRect(itemLeft, itemTop, itemLeft + textSizeInputWidth, itemTop + buttonSize);
    itemLeft = layout.textSizeInputRect.right() + itemGap;
    layout.fontDropdownRect = MakeRect(itemLeft, itemTop, itemLeft + dropdownWidth, itemTop + buttonSize);

    if (fontMenuOpen) {
        constexpr qreal optionHeight = 28.0;   // Layout.cpp:366
        constexpr qreal menuPadding = 6.0;     // Layout.cpp:367
        constexpr qreal menuGap = 6.0;         // Layout.cpp:368
        const qreal menuHeight = (menuPadding * 2.0) + (optionHeight * static_cast<qreal>(layout.fontOptionRects.size()));
        const qreal maxMenuTop = std::max(minTop, canvasRect.bottom() - menuHeight - canvasPadding);
        const qreal belowTop = std::clamp(layout.panelRect.bottom() + menuGap, minTop, maxMenuTop);
        const qreal aboveTop = std::clamp(layout.panelRect.top() - menuHeight - menuGap, minTop, maxMenuTop);
        const QRectF belowRect = MakeRect(layout.fontDropdownRect.left(), belowTop, layout.fontDropdownRect.right(), belowTop + menuHeight);
        const bool canUseBelow = !RectsOverlap(belowRect, editRect)
            && belowRect.bottom() <= canvasRect.bottom() - canvasPadding;
        const qreal menuTop = canUseBelow ? belowTop : aboveTop;
        layout.fontMenuRect = MakeRect(layout.fontDropdownRect.left(), menuTop, layout.fontDropdownRect.right(), menuTop + menuHeight);
        for (std::size_t index = 0; index < layout.fontOptionRects.size(); ++index) {
            const qreal optionTop = layout.fontMenuRect.top() + menuPadding + (static_cast<qreal>(index) * optionHeight);
            layout.fontOptionRects[index] = MakeRect(
                layout.fontMenuRect.left() + menuPadding,
                optionTop,
                layout.fontMenuRect.right() - menuPadding,
                optionTop + optionHeight);
        }
    }

    return layout;
}

QRectF TextFormatPopupBounds(const TextFormatPopupLayout& layout) {
    QRectF bounds = layout.panelRect;
    if (HasRect(layout.fontMenuRect)) {
        bounds = bounds.united(layout.fontMenuRect);
    }
    return bounds.adjusted(-3.0, -3.0, 3.0, 3.0);  // Layout.cpp:418
}

PaletteMenuLayout BuildPaletteMenuLayout(const ToolbarLayout& toolbarLayout, std::size_t colorCount) {
    PaletteMenuLayout layout;
    const std::size_t entryCount = colorCount + 1U;
    constexpr int maxColumns = 4;                 // Layout.cpp:377
    const int columns = std::min(maxColumns, std::max(1, static_cast<int>(entryCount)));
    constexpr qreal swatchSize = 28.0;            // Layout.cpp:378
    constexpr qreal gap = 8.0;                    // Layout.cpp:379
    const int rows = std::max(1, static_cast<int>((entryCount + static_cast<std::size_t>(columns) - 1U) / static_cast<std::size_t>(columns)));
    constexpr qreal panelPadding = 12.0;          // Layout.cpp:381
    const qreal panelWidth = (static_cast<qreal>(columns) * swatchSize) + (static_cast<qreal>(columns - 1) * gap) + (panelPadding * 2.0);
    const qreal panelHeight = (static_cast<qreal>(rows) * swatchSize) + (static_cast<qreal>(rows - 1) * gap) + (panelPadding * 2.0);
    layout.panelRect = MakeRect(
        toolbarLayout.currentColorRect.right() - panelWidth,
        toolbarLayout.currentColorRect.bottom() + 8.0,
        toolbarLayout.currentColorRect.right(),
        toolbarLayout.currentColorRect.bottom() + 8.0 + panelHeight);

    layout.swatchRects.reserve(colorCount);
    for (std::size_t index = 0; index < colorCount; ++index) {
        const int column = static_cast<int>(index % static_cast<std::size_t>(columns));
        const int row = static_cast<int>(index / static_cast<std::size_t>(columns));
        const qreal left = layout.panelRect.left() + panelPadding + (static_cast<qreal>(column) * (swatchSize + gap));
        const qreal top = layout.panelRect.top() + panelPadding + (static_cast<qreal>(row) * (swatchSize + gap));
        layout.swatchRects.push_back(MakeRect(left, top, left + swatchSize, top + swatchSize));
    }
    const int pickerColumn = static_cast<int>(colorCount % static_cast<std::size_t>(columns));
    const int pickerRow = static_cast<int>(colorCount / static_cast<std::size_t>(columns));
    const qreal pickerLeft = layout.panelRect.left() + panelPadding + (static_cast<qreal>(pickerColumn) * (swatchSize + gap));
    const qreal pickerTop = layout.panelRect.top() + panelPadding + (static_cast<qreal>(pickerRow) * (swatchSize + gap));
    layout.palettePickerRect = MakeRect(pickerLeft, pickerTop, pickerLeft + swatchSize, pickerTop + swatchSize);

    return layout;
}

PaletteWheelLayout BuildPaletteWheelLayout(const QRectF& clientRect, const QPointF& requestedCenter, std::size_t colorCount, qreal toolbarHeight) {
    PaletteWheelLayout layout;
    layout.innerRadius = 20.0;                                     // Layout.cpp:373
    layout.outerRadius = colorCount <= 1U ? 40.0 : 86.0;           // Layout.cpp:374
    layout.hitOuterRadius = layout.outerRadius + kPaletteWheelHoverGrowDip;
    const qreal margin = layout.hitOuterRadius
        + kPaletteWheelShadowInsetDip
        + std::ceil(std::max(1.0, kPaletteWheelSelectedStrokeDip))
        + 4.0;                                                     // Layout.cpp:376-379
    layout.center.setX(std::clamp(requestedCenter.x(), margin, clientRect.right() - margin));
    layout.center.setY(std::clamp(requestedCenter.y(), toolbarHeight + margin, clientRect.bottom() - margin));
    return layout;
}

std::optional<std::size_t> PaletteWheelIndexAtPoint(const PaletteWheelLayout& layout, const QPointF& cursor, std::size_t colorCount) {
    if (colorCount == 0U) {
        return std::nullopt;
    }

    const qreal dx = cursor.x() - layout.center.x();
    const qreal dy = cursor.y() - layout.center.y();
    const qreal distanceSquared = (dx * dx) + (dy * dy);
    const qreal innerSquared = layout.innerRadius * layout.innerRadius;
    const qreal hitOuterRadius = layout.hitOuterRadius > 0.0 ? layout.hitOuterRadius : layout.outerRadius;
    const qreal outerSquared = hitOuterRadius * hitOuterRadius;

    if (distanceSquared < innerSquared || distanceSquared > outerSquared) {
        return std::nullopt;
    }
    if (colorCount == 1U) {
        return 0U;
    }

    // The reference computes this in float (Layout.cpp:432-441); the port keeps
    // the same formula in qreal, so only a cursor exactly on a slice boundary
    // can differ by one index.
    const qreal wheelPi = WheelPi();
    qreal angle = std::atan2(dy, dx);
    angle += wheelPi / 2.0;
    if (angle < 0.0) {
        angle += 2.0 * wheelPi;
    }

    const qreal normalized = angle / (2.0 * wheelPi);
    const std::size_t index = std::min(
        colorCount - 1U,
        static_cast<std::size_t>(std::floor(normalized * static_cast<qreal>(colorCount))));
    return index;
}

bool IsToolbarControlRect(const ToolbarLayout& layout, const ToolbarTitleLayout& titleLayout, const QPointF& cursor) {
    // Same 12-control list and order as Layout.cpp:444-462.
    const std::array<QRectF, 12> controls {
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
    for (const QRectF& rect : controls) {
        if (ContainsPoint(rect, cursor)) {
            return true;
        }
    }
    return ContainsPoint(layout.eraseToolRect, cursor) || ContainsPoint(layout.currentColorRect, cursor);
}

QRectF ToolbarBrushTrackRect(const QRectF& sliderRect) {
    const qreal centerY = LegacyDiv(sliderRect.top() + sliderRect.bottom(), 2.0);
    return MakeRect(sliderRect.left() + 10.0, centerY - 3.0, sliderRect.right() - 10.0, centerY + 3.0);  // Layout.cpp:454-459
}

qreal ToolbarBrushThumbCenterX(const QRectF& sliderRect, float brushSize, float maxBrushSize) {
    const qreal width = std::max(1.0, std::trunc(sliderRect.width()));  // Layout.cpp:462
    const float sliderMax = std::max(1.0f, maxBrushSize);
    const float span = std::max(0.001f, sliderMax - 0.5f);
    const float normalized = std::clamp((std::min(SnapBrushSize(brushSize), sliderMax) - 0.5f) / span, 0.0f, 1.0f);
    return sliderRect.left() + std::lround(static_cast<qreal>(normalized) * (width - 1.0));
}

qreal ReservedToolbarHeight(bool autoHide, bool maximized) {
    return autoHide ? 0.0 : ToolbarHeight(maximized);
}

ToolbarAutoHide ResolveToolbarAutoHide(qreal cursorY, bool maximized, bool revealed, bool interactionActive, qreal frameThicknessX) {
    // App::UpdateToolbarAutoHide (App.cpp:2442-2479): an active drag/editor
    // returns without changing visibility; Show/Hide are the only transitions.
    if (interactionActive) {
        return ToolbarAutoHide::None;
    }

    const qreal revealTop = maximized ? -frameThicknessX : 0.0;
    const qreal revealHeight = maximized ? kMaximizedRevealHeight : kNormalRevealHeight;
    if (cursorY >= revealTop && cursorY < revealHeight) {
        return ToolbarAutoHide::Reveal;
    }

    const qreal toolbarHeight = ToolbarHeight(maximized);
    if (revealed && cursorY < toolbarHeight) {
        return ToolbarAutoHide::None;
    }
    if (revealed) {
        return ToolbarAutoHide::Hide;
    }
    return ToolbarAutoHide::None;
}

}  // namespace cnote::appui::qt
