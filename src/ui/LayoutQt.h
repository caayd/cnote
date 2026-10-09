#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include <QPointF>
#include <QRectF>

class QFont;

namespace cnote::appui::qt {

// Qt logical-pixel port of the reference application chrome (migration plan
// step 6.1).
//
// Source of truth: the *active* legacy call paths, not an arbitrary copy.  The
// Win32 frontend contains two copies of this layout and they disagree in
// exactly one behavioural constant:
//
//   * src/App.cpp keeps an anonymous-namespace copy that its own paint and
//     hit-test methods call, e.g. App::PaintPaletteWheel (App.cpp:6755),
//     App::PaintToolbar (App.cpp:6523), MessageBox-free title hit tests
//     (App.cpp:5912).
//   * src/ui/Layout.cpp (namespace cnote::appui) holds a second copy that
//     src/shell/MessageLoop.cpp and src/shell/ToolbarState.cpp call.
//
// The duplicate that disagrees is the palette-wheel hover grow constant: the
// App.cpp paint copy uses 12 (App.cpp:159) while the appui interaction copy
// uses 8 (Layout.cpp:26) for its hit radius and clamp margin.  This port
// resolves it by role instead of forcing one number onto both:
//
//   * BuildPaletteWheelLayout()'s hitOuterRadius/clamp margin follows the appui
//     interaction copy - the call path used by App::UpdatePaletteWheelHover
//     (ToolbarState.cpp:202), App::PaletteWheelBounds (ToolbarState.cpp:161)
//     and the message loop wheel hit test (MessageLoop.cpp:415).
//   * PaletteWheelHoverGrow()/PaletteWheelHoverOffset() expose the paint-only
//     hover feedback geometry from the App.cpp paint path
//     (App.cpp:158-159, 6773-6774).
//
// Every other constant matches both copies; the layout_parity_qt CTest drives
// cnote::appui directly on Windows to keep that true.
//
// All values are logical pixels at the 96-DPI reference scale, i.e. exactly the
// legacy ScaleForDpi(value, 96) result.  Qt already scales logical pixels for
// the display, so callers multiply by devicePixelRatioF() only when allocating
// rasters.  Text is measured on a fixed 96-DPI reference device so the system
// devicePixelRatio cannot rescale chrome text (plan step 3.6).
//
// Rect edges follow the legacy RECT convention: right()/bottom() are exclusive
// edges (width() == right() - left()).

struct ToolbarLayout {
    QRectF titleAreaRect;
    QRectF minimizeRect;
    QRectF maximizeRect;
    QRectF closeRect;
    QRectF undoRect;
    QRectF redoRect;
    QRectF selectToolRect;
    QRectF textToolRect;
    QRectF moveToolRect;
    QRectF drawToolRect;
    QRectF panToolRect;
    QRectF eraseToolRect;
    QRectF currentColorRect;
};

struct ToolbarTitleLayout {
    QRectF fileTitleRect;
    QRectF titleTextRect;
    QRectF zoomRect;
    QRectF zoomTextRect;
    qreal dividerX = 0.0;
};

struct SidebarMetrics {
    qreal width = 0.0;
    qreal itemInset = 0.0;
    qreal contentPadding = 0.0;
    qreal itemGap = 0.0;
    qreal labelHeight = 0.0;
    qreal minThumbHeight = 0.0;
    qreal placeholderInset = 0.0;
    qreal placeholderLineGap = 0.0;
};

// The reference's previousPageRect/nextPageRect are never assigned by
// cnote::appui::BuildSidebarHeaderLayout (Layout.cpp:226-241); they are omitted
// here rather than carried as permanently empty duplicates.
struct SidebarHeaderLayout {
    QRectF headerRect;
    QRectF pageLabelRect;
    QRectF addPageRect;
    QRectF newPageSettingsRect;
    QRectF contentRect;
};

struct QuickSizePopupLayout {
    QRectF panelRect;
    QRectF previewRect;
    QRectF sliderRect;
    QRectF valueRect;
};

struct TextFormatPopupLayout {
    QRectF panelRect;
    QRectF alignCycleRect;
    QRectF boldRect;
    QRectF italicRect;
    QRectF underlineRect;
    // Static small/medium/large text size: a dropdown like the font menu,
    // replacing the legacy free-form size input.
    QRectF sizeDropdownRect;
    QRectF fontDropdownRect;
    QRectF fontMenuRect;
    std::array<QRectF, 4> fontOptionRects;
    QRectF sizeMenuRect;
    std::array<QRectF, 3> sizeOptionRects;  // one per text-size preset
};

struct PaletteMenuLayout {
    QRectF panelRect;
    std::vector<QRectF> swatchRects;
    QRectF palettePickerRect;
};

struct PaletteWheelLayout {
    QPointF center;
    qreal innerRadius = 0.0;
    qreal outerRadius = 0.0;
    qreal hitOuterRadius = 0.0;
};

// Toolbar auto-hide trigger result (App.cpp:2442-2479
// App::UpdateToolbarAutoHide, the path driven on every mouse move).
// `None` preserves the reference's early returns: no state change.
enum class ToolbarAutoHide {
    None,
    Reveal,
    Hide,
};

// 34 logical-pixel base toolbar, 39 when maximized (Layout.cpp:15-16,58-60).
qreal ToolbarHeight(bool maximized);
// 5 logical-pixel maximized inset (Layout.cpp:62-64).
qreal ToolbarTopInset(bool maximized);
// (35 * 3) + 12 reserved for the in-client minimize/maximize/close buttons
// (Layout.cpp:42-44).  The Qt shell keeps the reference cluster position even
// though native decorations supply the buttons (plan step 6.3).
qreal CaptionButtonReserve();

// Paint-only palette-wheel hover feedback (App.cpp:158-159, 6773-6774).
qreal PaletteWheelHoverGrow();
qreal PaletteWheelHoverOffset();
qreal PaletteWheelIconHalfSize();

SidebarMetrics BuildSidebarMetrics();

// `font` is measured on the 96-DPI reference device; see the file comment.
ToolbarTitleLayout ResolveToolbarTitleLayout(const ToolbarLayout& layout, const QFont& font, const QString& title, const QString& zoomLabel);

// The exact measurement the title layout uses (empty text is 0, fixed 96-DPI
// reference device).  Exposed so the parity harness can report the
// Win32-vs-Qt advance difference it bounds.
qreal MeasureChromeTextWidth(const QFont& font, const QString& text);

// Parity seam: same clamp/edge arithmetic with caller-supplied measured text
// advances, so the layout can be compared against the reference on platforms
// that cannot run the Win32 GetTextExtentPoint32W path.
ToolbarTitleLayout ResolveToolbarTitleLayoutWithWidths(const ToolbarLayout& layout, qreal measuredTitleWidth, qreal measuredZoomWidth);

// `frameThicknessX` is the legacy Win32 client-area frame inset
// (appui::WindowFrameThicknessX); the Qt shell passes 0 because decorations are
// native, while the parity harness passes the reference value so the maximized
// arithmetic can be compared exactly.
ToolbarLayout BuildToolbarLayout(const QRectF& clientRect, std::size_t colorCount, bool maximized, int activeTool, qreal frameThicknessX = 0.0);
SidebarHeaderLayout BuildSidebarHeaderLayout(const QRectF& sidebarRect, bool maximized);
QuickSizePopupLayout BuildQuickSizePopupLayout(const QRectF& clientRect, const QPointF& anchor, qreal previewExtent);
QRectF QuickSizePopupBounds(const QuickSizePopupLayout& layout);
TextFormatPopupLayout BuildTextFormatPopupLayout(const QRectF& canvasRect, const QRectF& editRect, bool fontMenuOpen, bool sizeMenuOpen);
QRectF TextFormatPopupBounds(const TextFormatPopupLayout& layout);
PaletteMenuLayout BuildPaletteMenuLayout(const ToolbarLayout& toolbarLayout, std::size_t colorCount);
PaletteWheelLayout BuildPaletteWheelLayout(const QRectF& clientRect, const QPointF& requestedCenter, std::size_t colorCount, qreal toolbarHeight);
std::optional<std::size_t> PaletteWheelIndexAtPoint(const PaletteWheelLayout& layout, const QPointF& cursor, std::size_t colorCount);

// Exclusive right/bottom edges, matching Win32 PtInRect (Layout.cpp:444-462).
bool IsToolbarControlRect(const ToolbarLayout& layout, const ToolbarTitleLayout& titleLayout, const QPointF& cursor);

QRectF ToolbarBrushTrackRect(const QRectF& sliderRect);
qreal ToolbarBrushThumbCenterX(const QRectF& sliderRect, float brushSize, float maxBrushSize = 20.0f);

// Toolbar strip that stays reserved for the toolbar; 0 while auto-hide owns the
// space (App.cpp:2320-2322 App::ReservedToolbarHeight).
qreal ReservedToolbarHeight(bool autoHide, bool maximized);
// Reveal/hide decision for a cursor position, given the current reveal state.
// `interactionActive` is the reference's guard list of active drags/editors
// (App.cpp:2444-2464) and produces ToolbarAutoHide::None.
ToolbarAutoHide ResolveToolbarAutoHide(qreal cursorY, bool maximized, bool revealed, bool interactionActive, qreal frameThicknessX = 0.0);

}  // namespace cnote::appui::qt
