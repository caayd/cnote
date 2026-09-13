#pragma once

#include <cstdint>

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>

namespace cnote::ui {

// Qt port of the reference application-chrome palette and paint helpers
// (migration plan step 6.1).  Every value is copied from the *active* legacy
// paint path recorded next to it; nothing here is a new design.
//
//   src/App.cpp App::PaintToolbarContents  (toolbar surface, icon tints)
//   src/App.cpp App::PaintPaletteMenu      (swatch menu)
//   src/App.cpp App::PaintPaletteWheel     (radial palette/tool wheel)
//   src/App.cpp App::PaintQuickSizePopup   (brush size popup)
//   src/App.cpp App::PaintTextFormatPopup  (text format popup)
//   src/App.cpp App::PaintPdfSidebarContents (sidebar)
//   src/ui/UiUtil.h                        (shared panel/button palette)

inline constexpr std::uint32_t kChromeToolbarBackground = 0xFF0F1218U;  // App.cpp:6478
inline constexpr std::uint32_t kChromeWindowBackground = 0xFF12161DU;   // UiUtil.h:12
inline constexpr std::uint32_t kChromePanelBorder = 0xFF2B3440U;        // App.cpp:6479 (43,52,64)
inline constexpr std::uint32_t kChromePanelFill = 0xFF121820U;          // UiUtil.h:13
inline constexpr std::uint32_t kChromePanelStroke = 0xFF2E3A4AU;        // UiUtil.h:14 (46,58,74)
inline constexpr std::uint32_t kChromeButtonFill = 0xFF1C222BU;         // UiUtil.h:15
inline constexpr std::uint32_t kChromeButtonFillHover = 0xFF26303DU;    // UiUtil.h:17
inline constexpr std::uint32_t kChromeButtonFillActive = 0xFF30445EU;   // UiUtil.h:16
inline constexpr std::uint32_t kChromeAccent = 0xFF5C89BBU;             // UiUtil.h:22
inline constexpr std::uint32_t kChromeTextPrimary = 0xFFECF0F5U;        // App.cpp:6480
inline constexpr std::uint32_t kChromeTextMuted = 0xFF9CA5B2U;          // App.cpp:6482
inline constexpr std::uint32_t kChromeIconDisabled = 0xFF666F7CU;       // App.cpp:6533
inline constexpr std::uint32_t kChromeIconSelected = 0xFF84BFFFU;       // App.cpp:6533
inline constexpr std::uint32_t kChromeSelectionBorder = 0xFFF4F7FCU;    // UiUtil.h:21
inline constexpr std::uint32_t kChromeCaptionHover = 0xFF3C4048U;       // App.cpp:6573
inline constexpr std::uint32_t kChromeCaptionCloseHover = 0xFFC42B1CU;  // App.cpp:6573
inline constexpr std::uint32_t kChromeCaptionIcon = 0xFFF8FAFCU;        // App.cpp:6577
inline constexpr std::uint32_t kChromeWheelBadge = 0xFFF4F7FCU;         // App.cpp:6548

inline constexpr std::uint32_t kChromeSidebarBackground = 0xFF12161DU;   // App.cpp:7070
inline constexpr std::uint32_t kChromeSidebarDivider = 0xFF2B3440U;      // App.cpp:7075
inline constexpr std::uint32_t kChromeSidebarBottomBar = 0xFF0F1218U;    // App.cpp:7090
inline constexpr std::uint32_t kChromeSidebarSelected = 0xFF1F2937U;     // App.cpp:7175
inline constexpr std::uint32_t kChromeThumbBorder = 0xFF475260U;         // App.cpp:7213
inline constexpr std::uint32_t kChromeThumbPlaceholder = 0xFFCDD3DCU;    // App.cpp:7188
inline constexpr std::uint32_t kChromeSidebarLabelSelected = 0xFFF4F7FCU;  // App.cpp:7218
inline constexpr std::uint32_t kChromeSidebarLabel = 0xFFBBC3CEU;          // App.cpp:7218

inline constexpr std::uint32_t kChromeSliderTrack = 0xFF293342U;         // App.cpp:7562
inline constexpr std::uint32_t kChromeSliderThumb = 0xFFF4F7FCU;         // App.cpp:7594
inline constexpr std::uint32_t kChromeSliderThumbBorder = 0xFF788AA2U;   // App.cpp:7596
inline constexpr std::uint32_t kChromeSliderThumbActiveBorder = 0xFFFFFFFFU;  // App.cpp:7596

inline constexpr std::uint32_t kChromeFormatButtonFill = 0xFF1D2530U;    // App.cpp:7647
inline constexpr std::uint32_t kChromeFormatButtonBorder = 0xFF3A4758U;  // App.cpp:7648
inline constexpr std::uint32_t kChromeFormatTextMuted = 0xFFB1BCCAU;     // App.cpp:7650
inline constexpr std::uint32_t kChromeFormatAccentFill = 0xFF2C4F77U;    // App.cpp:7656
inline constexpr std::uint32_t kChromePalettePicker = 0xFF242E3AU;       // App.cpp:6725

// Reference rounded-rect radius (src/ui/UiUtil.cpp:281 CreateRoundedRectPath 6.0f).
inline constexpr qreal kChromeCornerRadius = 6.0;

// Chrome text uses the platform application font at the reference 15
// logical-pixel body size / 13 logical-pixel label size (App.cpp:6485-6488
// FontHeightForDpi(15|13)).  Plan step 3.7 keeps the platform UI family.
inline QFont ChromeFont(int pixelSize) {
    QFont font = QApplication::font();
    font.setPixelSize(pixelSize);
    return font;
}

inline QColor ChromeColor(std::uint32_t argb) {
    return QColor::fromRgba(argb);
}

inline QPainterPath ChromeRoundedPath(const QRectF& rect, qreal radius = kChromeCornerRadius) {
    QPainterPath path;
    const qreal clamped = qMin(radius, qMin(rect.width(), rect.height()) * 0.5);
    path.addRoundedRect(rect, clamped, clamped);
    return path;
}

inline void ChromeFillRoundedRect(QPainter& painter, const QRectF& rect, const QColor& color, qreal radius = kChromeCornerRadius) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPath(ChromeRoundedPath(rect, radius));
}

inline void ChromeStrokeRoundedRect(QPainter& painter, const QRectF& rect, const QColor& color, qreal width = 1.0, qreal radius = kChromeCornerRadius) {
    painter.setBrush(Qt::NoBrush);
    QPen pen(color, width);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawPath(ChromeRoundedPath(rect, radius));
}

inline void ChromePanel(QPainter& painter, const QRectF& rect) {
    ChromeFillRoundedRect(painter, rect, ChromeColor(kChromePanelFill));
    ChromeStrokeRoundedRect(painter, rect, ChromeColor(kChromePanelStroke), 1.0);
}

}  // namespace cnote::ui
