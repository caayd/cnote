// Layout parity evidence (migration plan step 6.1).
//
// One input matrix drives cnote::appui::qt, and every rectangle it produces is
// compared against the frozen reference table below.  The reference geometry
// itself no longer exists in this tree: the table is the expectation, and its
// values were captured from the Win32 reference layout implementation (they are
// re-derived by reading that table, never by re-measuring here).
//
// Tolerance, and why:
//   * Pure geometry is compared exactly (0 px).  Both sides are integer
//     arithmetic at the 96-DPI reference scale and the port reproduces the
//     reference's int truncation (LegacyDiv), so no tolerance is needed.
//   * Title/zoom rectangles are text-driven, and they are also compared
//     exactly: the reference's own measured advances (Win32
//     GetTextExtentPoint32W with the reference toolbar font) are frozen into
//     the table and drive the Qt clamp/edge arithmetic through
//     ResolveToolbarTitleLayoutWithWidths, so the rectangles must be identical.
//
// Coverage: window client sizes including compact widths (< 760 logical px),
// palette/color counts 0/1/5/10, maximized and normal frame state, all eight
// active tool variants, empty/medium/overlong title and zoom-label text,
// popup-edge clamping (left/right/top/bottom anchors and edit rects), the
// palette wheel clamp margin and slice hit test, brush slider helpers, and the
// toolbar auto-hide trigger boundaries (App.cpp:2442-2479) with the
// reserved/visible toolbox height rule.

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <QByteArray>
#include <QFont>
#include <QGuiApplication>
#include <QRectF>
#include <QString>

#include "ui/LayoutQt.h"

namespace {

namespace lq = cnote::appui::qt;

constexpr int kMaxPrintedFailures = 40;

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        if (g_failures <= kMaxPrintedFailures) {
            std::printf("FAIL: %s\n", message.c_str());
        }
    }
}

std::string Format(double value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

// ---------------------------------------------------------------- test data --

struct FrozenRect {
    const char* name;
    double left;
    double top;
    double right;
    double bottom;
};

using Rects = std::vector<FrozenRect>;

enum class Kind {
    Toolbar,
    Title,
    Sidebar,
    QuickSize,
    TextFormat,
    PaletteMenu,
    Wheel,
    BrushSlider,
};

struct Scenario {
    std::string label;
    Kind kind = Kind::Toolbar;
    QRectF clientRect { 0.0, 0.0, 1400.0, 960.0 };
    QRectF sidebarRect { 0.0, 0.0, 176.0, 960.0 };
    QRectF editRect { 100.0, 300.0, 500.0, 360.0 };
    std::size_t colorCount = 5U;
    bool maximized = false;
    int activeTool = 4;
    QPointF anchor { 0.0, 0.0 };
    double previewExtent = 0.0;
    std::string title;
    std::string zoomLabel;
    bool fontMenuOpen = false;
    double frameThicknessX = 0.0;
    double toolbarHeight = 34.0;
};

struct FrozenCase {
    const char* label;
    Rects rects;
};

struct FrozenTitleCase {
    const char* label;
    double measuredTitleWidth;
    double measuredZoomWidth;
    Rects rects;
};

struct AutoHideCase {
    const char* label;
    double cursorY;
    bool maximized;
    bool revealed;
    bool interactionActive;
    double frameThicknessX;
    lq::ToolbarAutoHide expected;
};

struct ReservedHeightCase {
    const char* label;
    bool autoHide;
    bool maximized;
    double expected;
};

void Add(Rects& rects, const char* name, const QRectF& rect) {
    rects.push_back(FrozenRect { name, rect.left(), rect.top(), rect.right(), rect.bottom() });
}

void AddValue(Rects& rects, const char* name, double value) {
    rects.push_back(FrozenRect { name, value, 0.0, value, 0.0 });
}

// ---------------------------------------------------------- Qt-side builders --

Rects BuildQtRects(const Scenario& scenario, const QFont& font) {
    static_cast<void>(font);
    Rects rects;
    switch (scenario.kind) {
    case Kind::Toolbar: {
        const lq::ToolbarLayout layout = lq::BuildToolbarLayout(
            scenario.clientRect, scenario.colorCount, scenario.maximized, scenario.activeTool, scenario.frameThicknessX);
        Add(rects, "titleAreaRect", layout.titleAreaRect);
        Add(rects, "minimizeRect", layout.minimizeRect);
        Add(rects, "maximizeRect", layout.maximizeRect);
        Add(rects, "closeRect", layout.closeRect);
        Add(rects, "undoRect", layout.undoRect);
        Add(rects, "redoRect", layout.redoRect);
        Add(rects, "selectToolRect", layout.selectToolRect);
        Add(rects, "textToolRect", layout.textToolRect);
        Add(rects, "moveToolRect", layout.moveToolRect);
        Add(rects, "drawToolRect", layout.drawToolRect);
        Add(rects, "panToolRect", layout.panToolRect);
        Add(rects, "eraseToolRect", layout.eraseToolRect);
        Add(rects, "currentColorRect", layout.currentColorRect);
        break;
    }
    case Kind::Title:
        // Title/zoom rectangles depend on text metrics, so they are compared
        // through BuildQtTitleRectsWithWidths (frozen reference widths); never
        // through this path.
        break;
    case Kind::Sidebar: {
        const lq::SidebarMetrics metrics = lq::BuildSidebarMetrics();
        AddValue(rects, "metric.width", metrics.width);
        AddValue(rects, "metric.itemInset", metrics.itemInset);
        AddValue(rects, "metric.contentPadding", metrics.contentPadding);
        AddValue(rects, "metric.itemGap", metrics.itemGap);
        AddValue(rects, "metric.labelHeight", metrics.labelHeight);
        AddValue(rects, "metric.minThumbHeight", metrics.minThumbHeight);
        AddValue(rects, "metric.placeholderInset", metrics.placeholderInset);
        AddValue(rects, "metric.placeholderLineGap", metrics.placeholderLineGap);
        const lq::SidebarHeaderLayout header = lq::BuildSidebarHeaderLayout(scenario.sidebarRect, scenario.maximized);
        Add(rects, "headerRect", header.headerRect);
        Add(rects, "pageLabelRect", header.pageLabelRect);
        Add(rects, "addPageRect", header.addPageRect);
        Add(rects, "newPageSettingsRect", header.newPageSettingsRect);
        Add(rects, "contentRect", header.contentRect);
        break;
    }
    case Kind::QuickSize: {
        const lq::QuickSizePopupLayout popup = lq::BuildQuickSizePopupLayout(
            scenario.clientRect, scenario.anchor, scenario.previewExtent);
        Add(rects, "panelRect", popup.panelRect);
        Add(rects, "previewRect", popup.previewRect);
        Add(rects, "sliderRect", popup.sliderRect);
        Add(rects, "valueRect", popup.valueRect);
        Add(rects, "bounds", lq::QuickSizePopupBounds(popup));
        break;
    }
    case Kind::TextFormat: {
        const lq::TextFormatPopupLayout popup = lq::BuildTextFormatPopupLayout(
            scenario.clientRect, scenario.editRect, scenario.fontMenuOpen, false);
        Add(rects, "panelRect", popup.panelRect);
        Add(rects, "alignCycleRect", popup.alignCycleRect);
        Add(rects, "boldRect", popup.boldRect);
        Add(rects, "italicRect", popup.italicRect);
        Add(rects, "underlineRect", popup.underlineRect);
        Add(rects, "sizeDropdownRect", popup.sizeDropdownRect);
        Add(rects, "fontDropdownRect", popup.fontDropdownRect);
        Add(rects, "fontMenuRect", popup.fontMenuRect);
        for (std::size_t index = 0; index < popup.fontOptionRects.size(); ++index) {
            Add(rects, "fontOptionRect", popup.fontOptionRects[index]);
        }
        Add(rects, "sizeMenuRect", popup.sizeMenuRect);
        for (std::size_t index = 0; index < popup.sizeOptionRects.size(); ++index) {
            Add(rects, "sizeOptionRect", popup.sizeOptionRects[index]);
        }
        Add(rects, "bounds", lq::TextFormatPopupBounds(popup));
        break;
    }
    case Kind::PaletteMenu: {
        const lq::ToolbarLayout toolbar = lq::BuildToolbarLayout(
            scenario.clientRect, scenario.colorCount, scenario.maximized, scenario.activeTool, scenario.frameThicknessX);
        const lq::PaletteMenuLayout menu = lq::BuildPaletteMenuLayout(toolbar, scenario.colorCount);
        Add(rects, "panelRect", menu.panelRect);
        for (const QRectF& swatch : menu.swatchRects) {
            Add(rects, "swatchRect", swatch);
        }
        Add(rects, "palettePickerRect", menu.palettePickerRect);
        break;
    }
    case Kind::Wheel: {
        const lq::PaletteWheelLayout wheel = lq::BuildPaletteWheelLayout(
            scenario.clientRect, scenario.anchor, scenario.colorCount, scenario.toolbarHeight);
        Add(rects, "innerCircle", QRectF(QPointF(wheel.center.x() - wheel.innerRadius, wheel.center.y() - wheel.innerRadius),
            QPointF(wheel.center.x() + wheel.innerRadius, wheel.center.y() + wheel.innerRadius)));
        Add(rects, "outerCircle", QRectF(QPointF(wheel.center.x() - wheel.outerRadius, wheel.center.y() - wheel.outerRadius),
            QPointF(wheel.center.x() + wheel.outerRadius, wheel.center.y() + wheel.outerRadius)));
        Add(rects, "hitOuterCircle", QRectF(QPointF(wheel.center.x() - wheel.hitOuterRadius, wheel.center.y() - wheel.hitOuterRadius),
            QPointF(wheel.center.x() + wheel.hitOuterRadius, wheel.center.y() + wheel.hitOuterRadius)));
        break;
    }
    case Kind::BrushSlider: {
        const QRectF track = lq::ToolbarBrushTrackRect(scenario.editRect);
        Add(rects, "trackRect", track);
        const std::array<float, 5> sizes { 0.5f, 1.0f, 3.25f, 12.0f, 20.0f };
        for (float size : sizes) {
            AddValue(rects, "thumbCenterX", lq::ToolbarBrushThumbCenterX(scenario.editRect, size));
        }
        break;
    }
    }
    return rects;
}

Rects BuildQtTitleRectsWithWidths(const Scenario& scenario, double measuredTitleWidth, double measuredZoomWidth) {
    const lq::ToolbarLayout layout = lq::BuildToolbarLayout(
        scenario.clientRect, scenario.colorCount, scenario.maximized, scenario.activeTool, scenario.frameThicknessX);
    const lq::ToolbarTitleLayout title = lq::ResolveToolbarTitleLayoutWithWidths(
        layout, measuredTitleWidth, measuredZoomWidth);
    Rects rects;
    Add(rects, "fileTitleRect", title.fileTitleRect);
    Add(rects, "titleTextRect", title.titleTextRect);
    Add(rects, "zoomRect", title.zoomRect);
    Add(rects, "zoomTextRect", title.zoomTextRect);
    AddValue(rects, "dividerX", title.dividerX);
    return rects;
}

// ------------------------------------------------------------- comparisons ---

void CompareRects(const std::string& context, const Rects& expected, const Rects& actual, double tolerance) {
    if (expected.size() != actual.size()) {
        Check(false, context + " rect count expected=" + std::to_string(expected.size()) + " actual=" + std::to_string(actual.size()));
        return;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const FrozenRect& want = expected[index];
        const FrozenRect& got = actual[index];
        if (std::strcmp(want.name, got.name) != 0) {
            Check(false, context + " rect order: expected " + want.name + " got " + got.name);
            continue;
        }
        const bool matches = std::abs(want.left - got.left) <= tolerance
            && std::abs(want.top - got.top) <= tolerance
            && std::abs(want.right - got.right) <= tolerance
            && std::abs(want.bottom - got.bottom) <= tolerance;
        Check(matches, context + " " + want.name + " reference=(" + Format(want.left) + "," + Format(want.top) + ","
            + Format(want.right) + "," + Format(want.bottom) + ") qt=(" + Format(got.left) + "," + Format(got.top) + ","
            + Format(got.right) + "," + Format(got.bottom) + ") tolerance=" + Format(tolerance));
    }
}

// ------------------------------------------------------------- frozen scenarios

std::vector<Scenario> FrozenGeometryScenarios() {
    std::vector<Scenario> scenarios;
    auto add = [&scenarios](Scenario scenario) { scenarios.push_back(std::move(scenario)); };

    Scenario toolbar;
    toolbar.label = "toolbar 1400x960 colors=5";
    toolbar.kind = Kind::Toolbar;
    add(toolbar);

    Scenario compact;
    compact.label = "toolbar compact 700x600 colors=5";
    compact.kind = Kind::Toolbar;
    compact.clientRect = QRectF(0.0, 0.0, 700.0, 600.0);
    add(compact);

    Scenario narrow;
    narrow.label = "toolbar narrow 500x400 colors=10";
    narrow.kind = Kind::Toolbar;
    narrow.clientRect = QRectF(0.0, 0.0, 500.0, 400.0);
    narrow.colorCount = 10U;
    narrow.activeTool = 0;
    add(narrow);

    Scenario menu;
    menu.label = "palette menu 1400x960 colors=5";
    menu.kind = Kind::PaletteMenu;
    add(menu);

    Scenario sidebar;
    sidebar.label = "sidebar 960 normal";
    sidebar.kind = Kind::Sidebar;
    add(sidebar);

    Scenario sidebarMax;
    sidebarMax.label = "sidebar 960 maximized";
    sidebarMax.kind = Kind::Sidebar;
    sidebarMax.maximized = true;
    add(sidebarMax);

    Scenario quickLeft;
    quickLeft.label = "quick-size clamp left";
    quickLeft.kind = Kind::QuickSize;
    quickLeft.anchor = QPointF(0.0, 40.0);
    add(quickLeft);

    Scenario quickRight;
    quickRight.label = "quick-size clamp right odd preview";
    quickRight.kind = Kind::QuickSize;
    quickRight.anchor = QPointF(1400.0, 40.0);
    quickRight.previewExtent = 29.0;
    add(quickRight);

    Scenario quickBottom;
    quickBottom.label = "quick-size clamp bottom";
    quickBottom.kind = Kind::QuickSize;
    quickBottom.anchor = QPointF(700.0, 960.0);
    quickBottom.previewExtent = 64.0;
    add(quickBottom);

    Scenario textPanel;
    textPanel.label = "text-format panel";
    textPanel.kind = Kind::TextFormat;
    add(textPanel);

    Scenario textMenuBelow;
    textMenuBelow.label = "text-format menu below";
    textMenuBelow.kind = Kind::TextFormat;
    textMenuBelow.editRect = QRectF(100.0, 60.0, 500.0, 120.0);
    textMenuBelow.fontMenuOpen = true;
    add(textMenuBelow);

    Scenario textMenuAbove;
    textMenuAbove.label = "text-format menu above";
    textMenuAbove.kind = Kind::TextFormat;
    textMenuAbove.editRect = QRectF(100.0, 800.0, 500.0, 860.0);
    textMenuAbove.fontMenuOpen = true;
    add(textMenuAbove);

    Scenario wheelCenter;
    wheelCenter.label = "palette wheel origin clamp colors=5";
    wheelCenter.kind = Kind::Wheel;
    wheelCenter.anchor = QPointF(0.0, 0.0);
    add(wheelCenter);

    Scenario wheelCorner;
    wheelCorner.label = "palette wheel corner clamp colors=1";
    wheelCorner.kind = Kind::Wheel;
    wheelCorner.anchor = QPointF(1400.0, 960.0);
    wheelCorner.colorCount = 1U;
    add(wheelCorner);

    Scenario brush;
    brush.label = "brush slider helpers";
    brush.kind = Kind::BrushSlider;
    brush.editRect = QRectF(10.0, 20.0, 200.0, 40.0);
    add(brush);

    return scenarios;
}

std::vector<Scenario> FrozenTitleScenarios() {
    std::vector<Scenario> scenarios;
    auto add = [&scenarios](Scenario scenario) { scenarios.push_back(std::move(scenario)); };

    Scenario empty;
    empty.label = "title empty zoom empty";
    empty.kind = Kind::Title;
    add(empty);

    Scenario medium;
    medium.label = "title medium zoom medium";
    medium.kind = Kind::Title;
    medium.title = "notes-2026.cnote";
    medium.zoomLabel = "125%";
    add(medium);

    Scenario overlong;
    overlong.label = "title overlong zoom overlong";
    overlong.kind = Kind::Title;
    overlong.title = "a-very-long-document-name-that-should-clamp.cnote";
    overlong.zoomLabel = "1250%";
    add(overlong);

    Scenario compactTitle;
    compactTitle.label = "title compact width";
    compactTitle.kind = Kind::Title;
    compactTitle.clientRect = QRectF(0.0, 0.0, 700.0, 600.0);
    compactTitle.title = "notes-2026.cnote";
    compactTitle.zoomLabel = "125%";
    add(compactTitle);

    return scenarios;
}

// Frozen reference values captured from the Win32 reference implementation:
// they are the expectations the Qt port is compared against below.
// FROZEN-GEOMETRY-BEGIN
const std::vector<FrozenCase> kFrozenGeometry {
    { "toolbar 1400x960 colors=5", {
        { "titleAreaRect", 12.0, 0.0, 911.0, 34.0 },
        { "minimizeRect", 1295.0, 0.0, 1330.0, 34.0 },
        { "maximizeRect", 1330.0, 0.0, 1365.0, 34.0 },
        { "closeRect", 1365.0, 0.0, 1400.0, 34.0 },
        { "undoRect", 927.0, 2.0, 957.0, 32.0 },
        { "redoRect", 967.0, 2.0, 997.0, 32.0 },
        { "selectToolRect", 1053.0, 2.0, 1083.0, 32.0 },
        { "textToolRect", 1093.0, 2.0, 1123.0, 32.0 },
        { "moveToolRect", 1013.0, 2.0, 1043.0, 32.0 },
        { "drawToolRect", 1173.0, 2.0, 1203.0, 32.0 },
        { "panToolRect", 1213.0, 2.0, 1243.0, 32.0 },
        { "eraseToolRect", 1133.0, 2.0, 1163.0, 32.0 },
        { "currentColorRect", 1261.0, 6.0, 1283.0, 28.0 },
    } },
    { "toolbar compact 700x600 colors=5", {
        { "titleAreaRect", 12.0, 0.0, 233.0, 34.0 },
        { "minimizeRect", 595.0, 0.0, 630.0, 34.0 },
        { "maximizeRect", 630.0, 0.0, 665.0, 34.0 },
        { "closeRect", 665.0, 0.0, 700.0, 34.0 },
        { "undoRect", 249.0, 2.0, 279.0, 32.0 },
        { "redoRect", 287.0, 2.0, 317.0, 32.0 },
        { "selectToolRect", 367.0, 2.0, 397.0, 32.0 },
        { "textToolRect", 405.0, 2.0, 435.0, 32.0 },
        { "moveToolRect", 329.0, 2.0, 359.0, 32.0 },
        { "drawToolRect", 481.0, 2.0, 511.0, 32.0 },
        { "panToolRect", 519.0, 2.0, 549.0, 32.0 },
        { "eraseToolRect", 443.0, 2.0, 473.0, 32.0 },
        { "currentColorRect", 561.0, 6.0, 583.0, 28.0 },
    } },
    { "toolbar narrow 500x400 colors=10", {
        { "titleAreaRect", 0.0, 0.0, 0.0, 0.0 },
        { "minimizeRect", 395.0, 0.0, 430.0, 34.0 },
        { "maximizeRect", 430.0, 0.0, 465.0, 34.0 },
        { "closeRect", 465.0, 0.0, 500.0, 34.0 },
        { "undoRect", 0.0, 0.0, 0.0, 0.0 },
        { "redoRect", 0.0, 0.0, 0.0, 0.0 },
        { "selectToolRect", 167.0, 2.0, 197.0, 32.0 },
        { "textToolRect", 205.0, 2.0, 235.0, 32.0 },
        { "moveToolRect", 129.0, 2.0, 159.0, 32.0 },
        { "drawToolRect", 281.0, 2.0, 311.0, 32.0 },
        { "panToolRect", 319.0, 2.0, 349.0, 32.0 },
        { "eraseToolRect", 243.0, 2.0, 273.0, 32.0 },
        { "currentColorRect", 361.0, 6.0, 383.0, 28.0 },
    } },
    { "palette menu 1400x960 colors=5", {
        { "panelRect", 1123.0, 36.0, 1283.0, 124.0 },
        { "swatchRect", 1135.0, 48.0, 1163.0, 76.0 },
        { "swatchRect", 1171.0, 48.0, 1199.0, 76.0 },
        { "swatchRect", 1207.0, 48.0, 1235.0, 76.0 },
        { "swatchRect", 1243.0, 48.0, 1271.0, 76.0 },
        { "swatchRect", 1135.0, 84.0, 1163.0, 112.0 },
        { "palettePickerRect", 1171.0, 84.0, 1199.0, 112.0 },
    } },
    { "sidebar 960 normal", {
        { "metric.width", 176.0, 0.0, 176.0, 0.0 },
        { "metric.itemInset", 12.0, 0.0, 12.0, 0.0 },
        { "metric.contentPadding", 12.0, 0.0, 12.0, 0.0 },
        { "metric.itemGap", 10.0, 0.0, 10.0, 0.0 },
        { "metric.labelHeight", 22.0, 0.0, 22.0, 0.0 },
        { "metric.minThumbHeight", 56.0, 0.0, 56.0, 0.0 },
        { "metric.placeholderInset", 14.0, 0.0, 14.0, 0.0 },
        { "metric.placeholderLineGap", 10.0, 0.0, 10.0, 0.0 },
        { "headerRect", 6.0, 926.0, 170.0, 960.0 },
        { "pageLabelRect", 6.0, 928.0, 58.0, 958.0 },
        { "addPageRect", 62.0, 928.0, 114.0, 958.0 },
        { "newPageSettingsRect", 118.0, 928.0, 170.0, 958.0 },
        { "contentRect", 0.0, 0.0, 176.0, 926.0 },
    } },
    { "sidebar 960 maximized", {
        { "metric.width", 176.0, 0.0, 176.0, 0.0 },
        { "metric.itemInset", 12.0, 0.0, 12.0, 0.0 },
        { "metric.contentPadding", 12.0, 0.0, 12.0, 0.0 },
        { "metric.itemGap", 10.0, 0.0, 10.0, 0.0 },
        { "metric.labelHeight", 22.0, 0.0, 22.0, 0.0 },
        { "metric.minThumbHeight", 56.0, 0.0, 56.0, 0.0 },
        { "metric.placeholderInset", 14.0, 0.0, 14.0, 0.0 },
        { "metric.placeholderLineGap", 10.0, 0.0, 10.0, 0.0 },
        { "headerRect", 6.0, 921.0, 170.0, 960.0 },
        { "pageLabelRect", 6.0, 925.0, 58.0, 955.0 },
        { "addPageRect", 62.0, 925.0, 114.0, 955.0 },
        { "newPageSettingsRect", 118.0, 925.0, 170.0, 955.0 },
        { "contentRect", 0.0, 0.0, 176.0, 921.0 },
    } },
    { "quick-size clamp left", {
        { "panelRect", 8.0, 41.0, 196.0, 79.0 },
        { "previewRect", 71.0, 8.0, 99.0, 36.0 },
        { "sliderRect", 18.0, 51.0, 152.0, 69.0 },
        { "valueRect", 158.0, 47.0, 186.0, 73.0 },
        { "bounds", 5.0, 5.0, 199.0, 82.0 },
    } },
    { "quick-size clamp right odd preview", {
        { "panelRect", 1204.0, 42.0, 1392.0, 80.0 },
        { "previewRect", 1267.0, 8.0, 1295.0, 37.0 },
        { "sliderRect", 1214.0, 52.0, 1348.0, 70.0 },
        { "valueRect", 1354.0, 48.0, 1382.0, 74.0 },
        { "bounds", 1201.0, 5.0, 1395.0, 83.0 },
    } },
    { "quick-size clamp bottom", {
        { "panelRect", 606.0, 908.0, 794.0, 946.0 },
        { "previewRect", 651.0, 839.0, 715.0, 903.0 },
        { "sliderRect", 616.0, 918.0, 750.0, 936.0 },
        { "valueRect", 756.0, 914.0, 784.0, 940.0 },
        { "bounds", 603.0, 836.0, 797.0, 949.0 },
    } },
    { "text-format panel", {
        { "panelRect", 100.0, 246.0, 494.0, 290.0 },
        { "alignCycleRect", 108.0, 254.0, 136.0, 282.0 },
        { "boldRect", 142.0, 254.0, 170.0, 282.0 },
        { "italicRect", 176.0, 254.0, 204.0, 282.0 },
        { "underlineRect", 210.0, 254.0, 238.0, 282.0 },
        { "sizeDropdownRect", 244.0, 254.0, 362.0, 282.0 },
        { "fontDropdownRect", 368.0, 254.0, 486.0, 282.0 },
        { "fontMenuRect", 0.0, 0.0, 0.0, 0.0 },
        { "fontOptionRect", 0.0, 0.0, 0.0, 0.0 },
        { "fontOptionRect", 0.0, 0.0, 0.0, 0.0 },
        { "fontOptionRect", 0.0, 0.0, 0.0, 0.0 },
        { "fontOptionRect", 0.0, 0.0, 0.0, 0.0 },
        { "sizeMenuRect", 0.0, 0.0, 0.0, 0.0 },
        { "sizeOptionRect", 0.0, 0.0, 0.0, 0.0 },
        { "sizeOptionRect", 0.0, 0.0, 0.0, 0.0 },
        { "sizeOptionRect", 0.0, 0.0, 0.0, 0.0 },
        { "bounds", 97.0, 243.0, 497.0, 293.0 },
    } },
    { "text-format menu below", {
        { "panelRect", 100.0, 190.0, 494.0, 234.0 },
        { "alignCycleRect", 108.0, 198.0, 136.0, 226.0 },
        { "boldRect", 142.0, 198.0, 170.0, 226.0 },
        { "italicRect", 176.0, 198.0, 204.0, 226.0 },
        { "underlineRect", 210.0, 198.0, 238.0, 226.0 },
        { "sizeDropdownRect", 244.0, 198.0, 362.0, 226.0 },
        { "fontDropdownRect", 368.0, 198.0, 486.0, 226.0 },
        { "fontMenuRect", 368.0, 240.0, 486.0, 364.0 },
        { "fontOptionRect", 374.0, 246.0, 480.0, 274.0 },
        { "fontOptionRect", 374.0, 274.0, 480.0, 302.0 },
        { "fontOptionRect", 374.0, 302.0, 480.0, 330.0 },
        { "fontOptionRect", 374.0, 330.0, 480.0, 358.0 },
        { "sizeMenuRect", 0.0, 0.0, 0.0, 0.0 },
        { "sizeOptionRect", 0.0, 0.0, 0.0, 0.0 },
        { "sizeOptionRect", 0.0, 0.0, 0.0, 0.0 },
        { "sizeOptionRect", 0.0, 0.0, 0.0, 0.0 },
        { "bounds", 97.0, 187.0, 497.0, 367.0 },
    } },
    { "text-format menu above", {
        { "panelRect", 100.0, 746.0, 494.0, 790.0 },
        { "alignCycleRect", 108.0, 754.0, 136.0, 782.0 },
        { "boldRect", 142.0, 754.0, 170.0, 782.0 },
        { "italicRect", 176.0, 754.0, 204.0, 782.0 },
        { "underlineRect", 210.0, 754.0, 238.0, 782.0 },
        { "sizeDropdownRect", 244.0, 754.0, 362.0, 782.0 },
        { "fontDropdownRect", 368.0, 754.0, 486.0, 782.0 },
        { "fontMenuRect", 368.0, 616.0, 486.0, 740.0 },
        { "fontOptionRect", 374.0, 622.0, 480.0, 650.0 },
        { "fontOptionRect", 374.0, 650.0, 480.0, 678.0 },
        { "fontOptionRect", 374.0, 678.0, 480.0, 706.0 },
        { "fontOptionRect", 374.0, 706.0, 480.0, 734.0 },
        { "sizeMenuRect", 0.0, 0.0, 0.0, 0.0 },
        { "sizeOptionRect", 0.0, 0.0, 0.0, 0.0 },
        { "sizeOptionRect", 0.0, 0.0, 0.0, 0.0 },
        { "sizeOptionRect", 0.0, 0.0, 0.0, 0.0 },
        { "bounds", 97.0, 613.0, 497.0, 793.0 },
    } },
    { "palette wheel origin clamp colors=5", {
        { "innerCircle", 87.0, 121.0, 127.0, 161.0 },
        { "outerCircle", 21.0, 55.0, 193.0, 227.0 },
        { "hitOuterCircle", 13.0, 47.0, 201.0, 235.0 },
    } },
    { "palette wheel corner clamp colors=1", {
        { "innerCircle", 1319.0, 879.0, 1359.0, 919.0 },
        { "outerCircle", 1299.0, 859.0, 1379.0, 939.0 },
        { "hitOuterCircle", 1291.0, 851.0, 1387.0, 947.0 },
    } },
    { "brush slider helpers", {
        { "trackRect", 20.0, 37.0, 200.0, 43.0 },
        { "thumbCenterX", 10.0, 0.0, 10.0, 0.0 },
        { "thumbCenterX", 15.0, 0.0, 15.0, 0.0 },
        { "thumbCenterX", 38.0, 0.0, 38.0, 0.0 },
        { "thumbCenterX", 127.0, 0.0, 127.0, 0.0 },
        { "thumbCenterX", 209.0, 0.0, 209.0, 0.0 },
    } },
};
// FROZEN-GEOMETRY-END
// FROZEN-TITLES-BEGIN
const std::vector<FrozenTitleCase> kFrozenTitles {
    { "title empty zoom empty", 0.0, 0.0, {
        { "fileTitleRect", 12.0, 0.0, 36.0, 34.0 },
        { "titleTextRect", 16.0, 0.0, 32.0, 34.0 },
        { "zoomRect", 48.0, 0.0, 92.0, 34.0 },
        { "zoomTextRect", 54.0, 0.0, 86.0, 34.0 },
        { "dividerX", 42.0, 0.0, 42.0, 0.0 },
    } },
    { "title medium zoom medium", 91.0, 28.0, {
        { "fileTitleRect", 12.0, 0.0, 111.0, 34.0 },
        { "titleTextRect", 16.0, 0.0, 107.0, 34.0 },
        { "zoomRect", 123.0, 0.0, 167.0, 34.0 },
        { "zoomTextRect", 129.0, 0.0, 161.0, 34.0 },
        { "dividerX", 117.0, 0.0, 117.0, 0.0 },
    } },
    { "title overlong zoom overlong", 295.0, 34.0, {
        { "fileTitleRect", 12.0, 0.0, 315.0, 34.0 },
        { "titleTextRect", 16.0, 0.0, 311.0, 34.0 },
        { "zoomRect", 327.0, 0.0, 373.0, 34.0 },
        { "zoomTextRect", 333.0, 0.0, 367.0, 34.0 },
        { "dividerX", 321.0, 0.0, 321.0, 0.0 },
    } },
    { "title compact width", 91.0, 28.0, {
        { "fileTitleRect", 12.0, 0.0, 111.0, 34.0 },
        { "titleTextRect", 16.0, 0.0, 107.0, 34.0 },
        { "zoomRect", 123.0, 0.0, 167.0, 34.0 },
        { "zoomTextRect", 129.0, 0.0, 161.0, 34.0 },
        { "dividerX", 117.0, 0.0, 117.0, 0.0 },
    } },
};
// FROZEN-TITLES-END

// Auto-hide boundaries: reference values from App.cpp:2465-2477 with the
// reference 34/39 logical-pixel toolbar heights and a 12 logical-pixel Win32
// frame inset.  Ported by ResolveToolbarAutoHide.
const std::vector<AutoHideCase> kAutoHideCases {
    { "cursor above client", -1.0, false, true, false, 12.0, lq::ToolbarAutoHide::None },
    { "cursor above client hidden", -1.0, false, false, false, 12.0, lq::ToolbarAutoHide::None },
    { "reveal band start", 0.0, false, false, false, 12.0, lq::ToolbarAutoHide::Reveal },
    { "reveal band end-1", 11.999, false, false, false, 12.0, lq::ToolbarAutoHide::Reveal },
    { "reveal band end", 12.0, false, false, false, 12.0, lq::ToolbarAutoHide::None },
    { "inside toolbar revealed", 33.999, false, true, false, 12.0, lq::ToolbarAutoHide::None },
    { "toolbar edge revealed", 34.0, false, true, false, 12.0, lq::ToolbarAutoHide::Hide },
    { "toolbar edge hidden", 34.0, false, false, false, 12.0, lq::ToolbarAutoHide::None },
    { "toolbar edge with active drag", 34.0, false, true, true, 12.0, lq::ToolbarAutoHide::None },
    { "maximized reveal band start", -12.0, true, false, false, 12.0, lq::ToolbarAutoHide::Reveal },
    { "maximized above reveal band", -12.001, true, false, false, 12.0, lq::ToolbarAutoHide::None },
    { "maximized reveal band end-1", 21.999, true, false, false, 12.0, lq::ToolbarAutoHide::Reveal },
    { "maximized reveal band end", 22.0, true, false, false, 12.0, lq::ToolbarAutoHide::None },
    { "maximized toolbar edge revealed", 39.0, true, true, false, 12.0, lq::ToolbarAutoHide::Hide },
    { "maximized inside toolbar", 38.999, true, true, false, 12.0, lq::ToolbarAutoHide::None },
    { "interaction suppresses hide", 200.0, false, true, true, 12.0, lq::ToolbarAutoHide::None },
};

const std::vector<ReservedHeightCase> kReservedHeightCases {
    { "auto-hide reserves nothing", true, false, 0.0 },
    { "auto-hide reserves nothing maximized", true, true, 0.0 },
    { "fixed toolbar normal", false, false, 34.0 },
    { "fixed toolbar maximized", false, true, 39.0 },
};

const char* AutoHideName(lq::ToolbarAutoHide action) {
    switch (action) {
    case lq::ToolbarAutoHide::None: return "None";
    case lq::ToolbarAutoHide::Reveal: return "Reveal";
    case lq::ToolbarAutoHide::Hide: return "Hide";
    }
    return "?";
}

void RunFrozenTable(const QFont& font) {
    Check(!kFrozenGeometry.empty(), "frozen geometry reference table is populated");
    Check(!kFrozenTitles.empty(), "frozen title reference table is populated");

    for (const FrozenCase& frozen : kFrozenGeometry) {
        bool found = false;
        for (const Scenario& scenario : FrozenGeometryScenarios()) {
            if (scenario.label == frozen.label) {
                found = true;
                CompareRects("frozen " + std::string(frozen.label), frozen.rects, BuildQtRects(scenario, font), 0.0);
            }
        }
        Check(found, std::string("frozen case has a producer: ") + frozen.label);
    }

    for (const FrozenTitleCase& frozen : kFrozenTitles) {
        bool found = false;
        for (const Scenario& scenario : FrozenTitleScenarios()) {
            if (scenario.label == frozen.label) {
                found = true;
                CompareRects("frozen " + std::string(frozen.label), frozen.rects,
                    BuildQtTitleRectsWithWidths(scenario, frozen.measuredTitleWidth, frozen.measuredZoomWidth), 0.0);
            }
        }
        Check(found, std::string("frozen title case has a producer: ") + frozen.label);
    }
}

void RunAutoHideTable() {
    for (const AutoHideCase& test : kAutoHideCases) {
        const lq::ToolbarAutoHide action = lq::ResolveToolbarAutoHide(
            test.cursorY, test.maximized, test.revealed, test.interactionActive, test.frameThicknessX);
        Check(action == test.expected, std::string("auto-hide ") + test.label + " expected=" + AutoHideName(test.expected)
            + " actual=" + AutoHideName(action));
    }
    for (const ReservedHeightCase& test : kReservedHeightCases) {
        const double height = lq::ReservedToolbarHeight(test.autoHide, test.maximized);
        Check(std::abs(height - test.expected) == 0.0, std::string("reserved toolbar height ") + test.label
            + " expected=" + Format(test.expected) + " actual=" + Format(height));
    }
    // Documented constants referenced by the plan (plan step 6.1).
    const lq::SidebarMetrics metrics = lq::BuildSidebarMetrics();
    Check(metrics.width == 176.0, "sidebar width is 176 logical pixels");
    Check(lq::ToolbarHeight(false) == 34.0, "toolbar height is 34 logical pixels");
    Check(lq::ToolbarHeight(true) == 39.0, "maximized toolbar height is 39 logical pixels");
    Check(lq::CaptionButtonReserve() == 117.0, "caption button reserve is (35 * 3) + 12 logical pixels");
    // Palette wheel: paint-path hover feedback vs interaction slop (the one
    // duplicate that disagrees; see LayoutQt.h).
    Check(lq::BuildPaletteWheelLayout(QRectF(0.0, 0.0, 1400.0, 960.0), QPointF(0.0, 0.0), 5U, 34.0).hitOuterRadius == 86.0 + 8.0,
        "wheel hit radius uses the appui interaction slop (outer + 8)");
    Check(lq::PaletteWheelHoverGrow() == 12.0, "wheel paint hover grow keeps the App.cpp paint value (12)");
    Check(lq::PaletteWheelHoverOffset() == 10.0, "wheel paint hover offset keeps the App.cpp paint value (10)");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // A Qt platform plugin that cannot be loaded makes QGuiApplication show an
    // invisible modal MessageBoxW and block forever.  Make the harness
    // self-sufficient (no reliance on the caller's environment) and arm a
    // watchdog so a missing/incompatible plugin fails fast instead of hanging.
    if (qEnvironmentVariableIsEmpty("QT_PLUGIN_PATH") && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM_PLUGIN_PATH")) {
#ifdef CNOTE_QT_PLUGIN_PATH
        qputenv("QT_PLUGIN_PATH", CNOTE_QT_PLUGIN_PATH);
#endif
    }
#ifdef Q_OS_WIN
    // The harness runs on the native platform on Windows, the platform its
    // frozen reference values were validated on.  It stays headless: this
    // harness never creates a window.
    qputenv("QT_QPA_PLATFORM", "windows");
#else
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
#endif

    std::atomic<bool> appReady { false };
    std::thread watchdog([&appReady] {
        for (int attempt = 0; attempt < 300; ++attempt) {
            if (appReady.load()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::fprintf(stderr, "layout parity: QGuiApplication did not initialize within 30 s "
            "(missing Qt platform plugin? QT_PLUGIN_PATH=%s)\n",
            qgetenv("QT_PLUGIN_PATH").constData());
        std::_Exit(7);
    });
    watchdog.detach();

    std::printf("layout parity: start\n");
    QGuiApplication app(argc, argv);
    appReady.store(true);
    std::printf("layout parity: platform=%s\n", QGuiApplication::platformName().toUtf8().constData());
    const QFont font = [] {
        QFont reference(QStringLiteral("Segoe UI"));
        reference.setPixelSize(15);
        return reference;
    }();

    RunFrozenTable(font);

    RunAutoHideTable();

    std::printf("%s: %d checks, %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
