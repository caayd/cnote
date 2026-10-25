// Deterministic offscreen contract test for the Qt compact chrome widgets
// (migration plan steps 6.1/6.2/6.3).
//
// Drives the real ToolbarWidget/SidebarWidget through Qt input events and
// defends the behavior the reference chrome actually has: control rectangles and
// hit regions against cnote::appui::qt for a width matrix including compact
// widths, hover state, palette-wheel color/tool intents, popup-edge clamping,
// sidebar page count/highlight/navigation, sidebar-local wheel isolation and
// elapsed-time auto-hide/slide transitions.  Every wait is bounded, so the test
// cannot block the suite.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QWheelEvent>
#include <QDir>
#include <QDirIterator>
#include <QPainter>
#include <QSvgRenderer>
#include <QWidget>

#include <optional>

#include "Document.h"
#include "settings/SettingsStoreQt.h"
#include "ui/ChromeAssetsQt.h"
#include "ui/ChromeStyleQt.h"
#include "ui/LayoutQt.h"
#include "ui/SidebarWidget.h"
#include "ui/ToolbarWidget.h"

namespace {

namespace appui = cnote::appui;
namespace qt = cnote::appui::qt;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void CheckNear(bool condition, const std::string& what, double actual, double expected, double tolerance) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s (actual=%.3f expected=%.3f tol=%.3f)\n", what.c_str(), actual, expected, tolerance);
    }
}

void SendMouse(QWidget& widget, QEvent::Type type, const QPointF& position, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, widget.mapToGlobal(position.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

void ClickWidget(QWidget& widget, const QPointF& position) {
    SendMouse(widget, QEvent::MouseButtonPress, position, Qt::LeftButton, Qt::LeftButton);
    SendMouse(widget, QEvent::MouseButtonRelease, position, Qt::LeftButton, Qt::NoButton);
}

void SendWheel(QWidget& widget, const QPointF& position, int delta) {
    QWheelEvent event(
        position,
        widget.mapToGlobal(position.toPoint()),
        QPoint(),
        QPoint(0, delta),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);
    QApplication::sendEvent(&widget, &event);
}

bool WaitFor(const std::function<bool()>& predicate, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return predicate();
}

cnote::Document BuildDocument(std::size_t pageCount) {
    std::vector<cnote::Page> pages;
    pages.reserve(pageCount);
    for (std::size_t index = 0; index < pageCount; ++index) {
        cnote::Page page;
        page.width = 612.0f;
        page.height = 792.0f;
        page.raster = cnote::CreateSolidRaster(page.width, page.height, 1.0f, index % 2U == 0U ? 0xFFFFFFFFU : 0xFFFFF1E0U);
        cnote::Stroke stroke;
        stroke.style.color = 0xFF204060U;
        stroke.style.width = 4.0f;
        for (int point = 0; point <= 20; ++point) {
            const float t = static_cast<float>(point) / 20.0f;
            stroke.points.push_back(cnote::Vec2 { 60.0f + (480.0f * t), 200.0f + (120.0f * std::sin(t * 3.14159265f)) });
        }
        page.strokes.push_back(std::move(stroke));
        pages.push_back(std::move(page));
    }
    return cnote::Document::CreateEditable(std::filesystem::path("chrome-test.cnote"), std::move(pages), 0, cnote::Document::PageDefaults {});
}

cnote::settings::Settings BuildSettings() {
    cnote::settings::Settings settings;
    settings.palette = cnote::settings::DefaultPalette();
    settings.colorIndex = 2;
    settings.drawBrushSize = 5.0f;
    settings.eraseBrushSize = 7.0f;
    settings.quickSizeSliderMax = 20.0f;
    settings.wheelTools = { cnote::settings::Tool::Draw, cnote::settings::Tool::Zoom };
    return settings;
}

QPointF SlicePoint(const qt::PaletteWheelLayout& layout, std::size_t index, std::size_t count) {
    constexpr double kPi = 3.14159265358979323846;
    const double fraction = (static_cast<double>(index) + 0.5) / static_cast<double>(count);
    const double angle = (fraction * 2.0 * kPi) - (kPi * 0.5);
    const double radius = (layout.innerRadius + layout.outerRadius) * 0.5;
    return QPointF(
        layout.center.x() + (std::cos(angle) * radius),
        layout.center.y() + (std::sin(angle) * radius));
}

void TestToolbarLayoutMatrix() {
    cnote::ui::ToolbarWidget toolbar;
    toolbar.SetSettings(BuildSettings());
    toolbar.SetTitle(QStringLiteral("representative.cnote"));
    toolbar.SetZoomLabel(QStringLiteral("100%"));
    toolbar.SetUndoRedoEnabled(true, true);
    toolbar.SetCaptionButtonsVisible(true);

    const std::vector<int> widths { 420, 520, 640, 700, 760, 900, 1200, 1400, 1600, 2000 };
    for (const bool maximized : { false, true }) {
        toolbar.SetMaximized(maximized);
        const qreal toolbarHeight = qt::ToolbarHeight(maximized);
        for (const int width : widths) {
            toolbar.resize(width, static_cast<int>(std::lround(toolbarHeight)));
            const qt::ToolbarLayout want = qt::BuildToolbarLayout(
                QRectF(0.0, 0.0, width, toolbarHeight), toolbar.Settings().palette.size(), maximized, static_cast<int>(toolbar.ActiveTool()), 0.0);
            const qt::ToolbarLayout got = toolbar.ComputeLayout();
            const std::string context = " (width=" + std::to_string(width) + " maximized=" + (maximized ? "1" : "0") + ")";

            Check(got.titleAreaRect == want.titleAreaRect, "titleAreaRect matches LayoutQt" + context);
            Check(got.undoRect == want.undoRect && got.redoRect == want.redoRect, "undo/redo rects match LayoutQt" + context);
            Check(got.selectToolRect == want.selectToolRect && got.textToolRect == want.textToolRect
                    && got.latexToolRect == want.latexToolRect, "select/text/latex rects match LayoutQt" + context);
            Check(got.moveToolRect == want.moveToolRect && got.eraseToolRect == want.eraseToolRect, "move/erase rects match LayoutQt" + context);
            Check(got.drawToolRect == want.drawToolRect && got.panToolRect == want.panToolRect, "draw/pan rects match LayoutQt" + context);
            Check(got.currentColorRect == want.currentColorRect, "color swatch rect matches LayoutQt" + context);
            Check(got.minimizeRect == want.minimizeRect && got.closeRect == want.closeRect, "caption rects match LayoutQt" + context);
        }
    }

    // Hit regions: each control's center resolves to that control, and one pixel
    // past its right edge does not (exclusive Win32 edges).
    toolbar.SetMaximized(false);
    toolbar.resize(1400, 34);
    const struct {
        cnote::ui::ToolbarControl control;
        const char* name;
    } controls[] = {
        { cnote::ui::ToolbarControl::Undo, "undo" },
        { cnote::ui::ToolbarControl::Redo, "redo" },
        { cnote::ui::ToolbarControl::SelectTool, "select" },
        { cnote::ui::ToolbarControl::MoveTool, "move" },
        { cnote::ui::ToolbarControl::TextTool, "text" },
        { cnote::ui::ToolbarControl::LatexTool, "latex" },
        { cnote::ui::ToolbarControl::DrawTool, "draw" },
        { cnote::ui::ToolbarControl::PanTool, "pan" },
        { cnote::ui::ToolbarControl::EraseTool, "erase" },
        { cnote::ui::ToolbarControl::CurrentColor, "color" },
        { cnote::ui::ToolbarControl::Zoom, "zoom" },
        { cnote::ui::ToolbarControl::TitleMenu, "title" },
        { cnote::ui::ToolbarControl::Minimize, "minimize" },
        { cnote::ui::ToolbarControl::Maximize, "maximize" },
        { cnote::ui::ToolbarControl::Close, "close" },
    };
    for (const auto& entry : controls) {
        const QRectF rect = toolbar.ControlRect(entry.control);
        if (!rect.isValid()) {
            std::printf("SKIP: %s control rect invalid at 1400px\n", entry.name);
            continue;
        }
        const std::string name = entry.name;
        Check(toolbar.ControlAt(rect.center()) == entry.control, std::string("hit test finds ") + name + " at its center");
        const QPointF outside(rect.right() + 0.5, rect.center().y());
        Check(toolbar.ControlAt(outside) != entry.control, std::string("hit test excludes ") + name + " right edge");
        if (rect.top() > 0.5) {
            const QPointF above(rect.center().x(), rect.top() - 0.5);
            Check(toolbar.ControlAt(above) != entry.control, std::string("hit test excludes ") + name + " top edge");
        }
    }

    // Layout equality for the resolved title/zoom rects at a non-compact width.
    const qt::ToolbarLayout layout = toolbar.ComputeLayout();
    const QFont body = cnote::ui::ChromeFont(15);
    const qt::ToolbarTitleLayout wantTitle = qt::ResolveToolbarTitleLayoutWithWidths(
        layout,
        qt::MeasureChromeTextWidth(body, toolbar.Title()),
        qt::MeasureChromeTextWidth(body, QStringLiteral("100%")));
    const qt::ToolbarTitleLayout gotTitle = toolbar.ComputeTitleLayout();
    Check(gotTitle.fileTitleRect == wantTitle.fileTitleRect, "title rect matches LayoutQt measurement");
    Check(gotTitle.zoomRect == wantTitle.zoomRect, "zoom rect matches LayoutQt measurement");
}

void TestToolbarIntentsAndHover() {
    QWidget host;
    host.resize(1400, 900);
    cnote::ui::ToolbarWidget* toolbar = new cnote::ui::ToolbarWidget(&host);
    toolbar->SetSettings(BuildSettings());
    toolbar->SetUndoRedoEnabled(true, true);
    toolbar->SetCaptionButtonsVisible(true);
    toolbar->resize(1400, 34);
    host.show();
    QApplication::processEvents();

    int undoCount = 0;
    int redoCount = 0;
    int closeCount = 0;
    std::optional<cnote::settings::Tool> selectedTool;
    QString committedZoom;
    QObject::connect(toolbar, &cnote::ui::ToolbarWidget::CommandTriggered, [&](cnote::ui::ToolbarCommand command) {
        if (command == cnote::ui::ToolbarCommand::Undo) {
            ++undoCount;
        } else if (command == cnote::ui::ToolbarCommand::Redo) {
            ++redoCount;
        } else if (command == cnote::ui::ToolbarCommand::Close) {
            ++closeCount;
        }
    });
    QObject::connect(toolbar, &cnote::ui::ToolbarWidget::ToolSelected, [&](cnote::settings::Tool tool) { selectedTool = tool; });
    QObject::connect(toolbar, &cnote::ui::ToolbarWidget::ZoomTextCommitted, [&](const QString& text) { committedZoom = text; });

    ClickWidget(*toolbar, toolbar->ControlRect(cnote::ui::ToolbarControl::Undo).center());
    ClickWidget(*toolbar, toolbar->ControlRect(cnote::ui::ToolbarControl::Redo).center());
    ClickWidget(*toolbar, toolbar->ControlRect(cnote::ui::ToolbarControl::Close).center());
    Check(undoCount == 1 && redoCount == 1, "undo/redo clicks emit their command intents");
    Check(closeCount == 1, "close click emits the close command intent");

    ClickWidget(*toolbar, toolbar->ControlRect(cnote::ui::ToolbarControl::DrawTool).center());
    Check(selectedTool.has_value() && *selectedTool == cnote::settings::Tool::Draw, "draw tool click emits ToolSelected(Draw)");
    Check(toolbar->ActiveTool() == cnote::settings::Tool::Draw, "draw tool click updates the widget's active tool");

    // Hover tracking.
    const QPointF drawCenter = toolbar->ControlRect(cnote::ui::ToolbarControl::DrawTool).center();
    SendMouse(*toolbar, QEvent::MouseMove, drawCenter, Qt::NoButton, Qt::NoButton);
    Check(toolbar->HoveredControl() == cnote::ui::ToolbarControl::DrawTool, "hovering the draw tool sets the hovered control");
    SendMouse(*toolbar, QEvent::MouseMove, QPointF(5.0, 5.0), Qt::NoButton, Qt::NoButton);
    Check(toolbar->HoveredControl() != cnote::ui::ToolbarControl::DrawTool, "moving off the draw tool clears the hovered control");

    // Zoom inline edit commits its text.
    toolbar->BeginZoomEdit();
    Check(toolbar->ZoomEditing(), "zoom edit begins");
    for (const auto* text : { "1", "5", "0" }) {
        QKeyEvent key(QEvent::KeyPress, Qt::Key_0, Qt::NoModifier, QString::fromLatin1(text));
        QApplication::sendEvent(toolbar, &key);
    }
    QKeyEvent commit(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(toolbar, &commit);
    Check(!toolbar->ZoomEditing(), "zoom edit commits and closes");
    Check(committedZoom == QStringLiteral("150"), "zoom edit reports the typed value");
}

void TestPaletteWheelIntents() {
    QWidget host;
    host.resize(1400, 900);
    cnote::ui::ToolbarWidget* toolbar = new cnote::ui::ToolbarWidget(&host);
    toolbar->SetSettings(BuildSettings());
    toolbar->resize(1400, 34);
    host.show();
    QApplication::processEvents();

    std::optional<std::size_t> selectedColor;
    std::optional<cnote::settings::Tool> selectedTool;
    QObject::connect(toolbar, &cnote::ui::ToolbarWidget::ColorSelected, [&](std::size_t index) { selectedColor = index; });
    QObject::connect(toolbar, &cnote::ui::ToolbarWidget::ToolSelected, [&](cnote::settings::Tool tool) { selectedTool = tool; });

    const std::vector<cnote::settings::Tool> displayed = toolbar->PaletteWheelDisplayedTools();
    const std::size_t entryCount = toolbar->Settings().palette.size() + displayed.size();
    Check(displayed.size() == 1U && displayed.front() == cnote::settings::Tool::Draw,
        "wheel entries exclude the zoom-only entry and keep the configured tools");
    Check(entryCount == toolbar->Settings().palette.size() + 1U && entryCount > 1U,
        "wheel entry count is the palette plus displayed tools");

    const QPointF requested(420.0, 320.0);
    const qt::PaletteWheelLayout layout = qt::BuildPaletteWheelLayout(QRectF(0.0, 0.0, 1400.0, 900.0), requested, entryCount, qt::ToolbarHeight(false));

    toolbar->TogglePaletteWheel(requested);
    Check(toolbar->PaletteWheelVisible(), "palette wheel opens");
    QWidget* wheel = host.findChild<QWidget*>(QStringLiteral("cnotePaletteWheel"));
    Check(wheel != nullptr && wheel->isVisible(), "palette wheel overlay is a visible child of the host");

    if (wheel != nullptr) {
        ClickWidget(*wheel, SlicePoint(layout, 3, entryCount));
        Check(selectedColor.has_value() && *selectedColor == 3U, "wheel color slice emits ColorSelected for that index");
        Check(!toolbar->PaletteWheelVisible(), "wheel closes after a color selection");
    }

    toolbar->TogglePaletteWheel(requested);
    wheel = host.findChild<QWidget*>(QStringLiteral("cnotePaletteWheel"));
    if (wheel != nullptr) {
        ClickWidget(*wheel, SlicePoint(layout, toolbar->Settings().palette.size(), entryCount));
        Check(selectedTool.has_value() && *selectedTool == cnote::settings::Tool::Draw, "wheel tool slice emits ToolSelected(Draw)");
        Check(!toolbar->PaletteWheelVisible(), "wheel closes after a tool selection");
    }

    // Clicking outside the wheel closes it and is consumed by the wheel overlay.
    toolbar->TogglePaletteWheel(requested);
    wheel = host.findChild<QWidget*>(QStringLiteral("cnotePaletteWheel"));
    if (wheel != nullptr) {
        const QPointF outside(layout.center.x() + layout.hitOuterRadius + 40.0, layout.center.y());
        SendMouse(*wheel, QEvent::MouseButtonPress, outside, Qt::LeftButton, Qt::LeftButton);
        SendMouse(*wheel, QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
        Check(!toolbar->PaletteWheelVisible(), "wheel closes on a click outside the ring");
    }
}

// The no-argument entry resolves the reference anchor itself
// (App::TogglePaletteWheel, ToolbarState.cpp:127-159): centred on the cursor
// when it is inside the window, otherwise 72 DIPs below the toolbar's
// current-colour swatch.  QCursor::setPos is the only way to drive it, and a
// platform may clamp it, so every expectation follows the cursor position read
// back from the host instead of the requested one.
void TestPaletteWheelAnchor() {
    QWidget host;
    host.resize(1400, 900);
    cnote::ui::ToolbarWidget* toolbar = new cnote::ui::ToolbarWidget(&host);
    toolbar->SetSettings(BuildSettings());
    toolbar->resize(1400, 34);
    host.show();
    QApplication::processEvents();

    const std::size_t entryCount = toolbar->Settings().palette.size() + toolbar->PaletteWheelDisplayedTools().size();
    const QRectF clientRect(0.0, 0.0, static_cast<qreal>(host.width()), static_cast<qreal>(host.height()));

    // The wheel's hit test is the public way to observe where it was centred:
    // drive a pointer position through the overlay and read the slice it lands on.
    const auto hoverAfterProbe = [&](const QPointF& probe) {
        std::optional<std::size_t> hover;
        if (QWidget* wheel = host.findChild<QWidget*>(QStringLiteral("cnotePaletteWheel")); wheel != nullptr) {
            SendMouse(*wheel, QEvent::MouseMove, probe, Qt::NoButton, Qt::NoButton);
            hover = toolbar->PaletteWheelHoverIndex();
        }
        return hover;
    };

    QCursor::setPos(host.mapToGlobal(QPoint(700, 500)));
    QApplication::processEvents();
    const QPointF cursorInHost(host.mapFromGlobal(QCursor::pos()));
    const bool cursorInside = cursorInHost.x() >= 0.0 && cursorInHost.y() >= 0.0
        && cursorInHost.x() < clientRect.width() && cursorInHost.y() < clientRect.height();
    if (cursorInside) {
        toolbar->TogglePaletteWheel();
        Check(toolbar->PaletteWheelVisible(), "the wheel opens without an explicit anchor");
        const qt::PaletteWheelLayout layout = qt::BuildPaletteWheelLayout(clientRect, cursorInHost, entryCount, qt::ToolbarHeight(false));
        Check(toolbar->PaletteWheelHoverIndex() == qt::PaletteWheelIndexAtPoint(layout, cursorInHost, entryCount),
            "the opened wheel seeds the hover slice under the cursor");
        const QPointF probe(layout.center.x(), layout.center.y() + 62.0);
        const std::optional<std::size_t> expected = qt::PaletteWheelIndexAtPoint(layout, probe, entryCount);
        Check(expected.has_value() && hoverAfterProbe(probe) == expected, "the wheel is centred on the cursor");
        toolbar->HidePaletteWheel();
    }

    // Moving the window off the pointer puts the cursor outside the host client
    // rect, which switches to the reference swatch fallback anchor.
    host.move(host.pos() + QPoint(host.width() + 400, 0));
    QApplication::processEvents();
    QCursor::setPos(QPoint(10, 10));
    QApplication::processEvents();
    const QPointF cursorOutside(host.mapFromGlobal(QCursor::pos()));
    const bool fallbackAnchor = cursorOutside.x() < 0.0 || cursorOutside.y() < 0.0
        || cursorOutside.x() >= clientRect.width() || cursorOutside.y() >= clientRect.height();
    if (fallbackAnchor) {
        const QPointF hostOrigin(toolbar->mapTo(&host, QPoint(0, 0)));
        const QRectF colorRect = toolbar->ControlRect(cnote::ui::ToolbarControl::CurrentColor).translated(hostOrigin);
        const QPointF anchor(colorRect.center().x(), colorRect.bottom() + 72.0);
        toolbar->TogglePaletteWheel();
        Check(toolbar->PaletteWheelVisible(), "the wheel opens with the swatch fallback anchor");
        const qt::PaletteWheelLayout layout = qt::BuildPaletteWheelLayout(clientRect, anchor, entryCount, qt::ToolbarHeight(false));
        const QPointF probe(layout.center.x(), layout.center.y() + 62.0);
        const std::optional<std::size_t> expected = qt::PaletteWheelIndexAtPoint(layout, probe, entryCount);
        Check(expected.has_value() && hoverAfterProbe(probe) == expected, "the fallback anchor sits below the colour swatch");
        toolbar->HidePaletteWheel();
        Check(!toolbar->PaletteWheelHoverIndex().has_value(), "closing the wheel clears the hover slice");
    }
}

void TestPopupPlacementAndClamping() {
    QWidget host;
    host.resize(1000, 700);
    cnote::ui::ToolbarWidget* toolbar = new cnote::ui::ToolbarWidget(&host);
    toolbar->SetSettings(BuildSettings());
    toolbar->resize(1000, 34);
    host.show();
    QApplication::processEvents();

    const QRect hostRect(0, 0, 1000, 700);
    const QPointF anchors[] {
        QPointF(500.0, 200.0),
        QPointF(2.0, 2.0),
        QPointF(998.0, 698.0),
        QPointF(4.0, 690.0),
        QPointF(996.0, 6.0),
    };
    for (const QPointF& anchor : anchors) {
        toolbar->ShowQuickSizePopup(cnote::settings::Tool::Draw, anchor);
        Check(toolbar->QuickSizePopupVisible(), "quick-size popup opens");
        const QRect bounds = toolbar->QuickSizePopupBounds();
        Check(hostRect.contains(bounds), "quick-size popup stays inside the window");
        Check(std::abs(toolbar->QuickSizeValue() - 5.0f) < 0.001f, "quick-size popup shows the draw brush size");

        // The reference preview extent for a 5 px draw brush at page scale 1 is
        // ceil(max(1.25, 5/2.15)) + 2 == 5 logical pixels.
        const qt::QuickSizePopupLayout want = qt::BuildQuickSizePopupLayout(
            QRectF(0.0, qt::ReservedToolbarHeight(false, false), 1000.0, 700.0 - qt::ReservedToolbarHeight(false, false)),
            anchor,
            5.0);
        const QRect wantBounds = qt::QuickSizePopupBounds(want).toAlignedRect();
        Check(toolbar->QuickSizePopupBounds() == wantBounds, "quick-size popup geometry matches LayoutQt");
        Check(hostRect.contains(wantBounds), "quick-size panel (LayoutQt) stays inside the window");
        toolbar->HideQuickSizePopup();
    }

    // Text-format popup clamping, above/below the edit rect and with the font
    // menu open, using the reference canvas rectangle.
    toolbar->SetTextFormatAvailable(true);
    cnote::PageTextStyle style;
    style.fontSize = 24.0f;
    style.bold = true;
    toolbar->SetTextFormatState(style, cnote::TextAlignment::Center, 1U);
    const QRectF canvasRect(0.0, 34.0, 1000.0, 666.0);
    const QRectF editRects[] {
        QRectF(400.0, 300.0, 200.0, 80.0),
        QRectF(20.0, 60.0, 160.0, 60.0),
        QRectF(820.0, 640.0, 160.0, 60.0),
    };
    for (const QRectF& editRect : editRects) {
        toolbar->ShowTextFormatPopup(editRect);
        Check(toolbar->TextFormatPopupVisible(), "text-format popup opens");
        Check(hostRect.contains(toolbar->TextFormatPopupBounds()), "text-format popup stays inside the window");
        const qt::TextFormatPopupLayout want = qt::BuildTextFormatPopupLayout(canvasRect, editRect, false, false);
        Check(hostRect.contains(want.panelRect.toAlignedRect()), "text-format panel (LayoutQt) stays inside the window");
        Check(want.panelRect.top() - 3.0 >= canvasRect.top() - 0.001 || want.panelRect.bottom() + 3.0 <= canvasRect.bottom() + 0.001,
            "text-format panel is clamped vertically into the canvas");
        toolbar->HideTextFormatPopup();
    }
}

void TestSidebarListAndNavigation() {
    QWidget host;
    host.resize(1400, 900);
    cnote::ui::SidebarWidget sidebar(&host);
    const cnote::Document document = BuildDocument(12);
    sidebar.SetDocument(&document, nullptr);
    sidebar.SetHostClientRect(host.rect(), 34);
    sidebar.SetOpen(true);
    host.show();
    QApplication::processEvents();

    Check(WaitFor([&]() { return sidebar.SlideProgress() >= 0.999; }, 3000), "sidebar slide animation completes within the bound");
    Check(sidebar.IsOpen(), "sidebar reports open");
    Check(sidebar.PageCount() == 12U, "sidebar reports the document page count");
    Check(sidebar.geometry().width() == 176, "sidebar is 176 logical pixels wide");
    Check(sidebar.geometry().x() == 0 && sidebar.geometry().y() == 34, "open sidebar sits at the window's left edge below the toolbar");

    const QRect content = sidebar.HeaderLayout().contentRect.toAlignedRect();
    Check(!content.isEmpty(), "sidebar content band exists above the header");
    Check(sidebar.HeaderLayout().headerRect.toAlignedRect().bottom() <= sidebar.geometry().height(),
        "sidebar header is inside the widget");

    for (std::size_t index = 0; index < sidebar.PageCount(); ++index) {
        Check(sidebar.PageItemRect(index).has_value(), "page item rect exists for page " + std::to_string(index));
    }

    const std::size_t visibleBefore = sidebar.VisibleThumbnailCount();
    Check(visibleBefore >= 1U, "at least one thumbnail is visible");

    const QPixmap grabbed = sidebar.grab();
    Check(!grabbed.isNull(), "sidebar paints offscreen");
    Check(sidebar.RenderedThumbnailCount() >= visibleBefore, "every visible page composes a thumbnail");
    Check(sidebar.HasThumbnail(0), "first page has a composed thumbnail");

    // Page navigation intent from a click on a visible item.
    std::optional<std::size_t> activated;
    QObject::connect(&sidebar, &cnote::ui::SidebarWidget::PageActivated, [&](std::size_t index) { activated = index; });
    std::optional<std::size_t> firstVisible;
    for (std::size_t index = 0; index < sidebar.PageCount(); ++index) {
        const std::optional<QRect> item = sidebar.PageItemRect(index);
        if (item.has_value() && item->bottom() >= content.top() && item->top() <= content.bottom()) {
            firstVisible = index;
            break;
        }
    }
    Check(firstVisible.has_value(), "a page item is inside the content band");
    if (firstVisible.has_value()) {
        const QRect item = *sidebar.PageItemRect(*firstVisible);
        ClickWidget(sidebar, QPointF(item.center()));
        Check(activated.has_value() && *activated == *firstVisible, "clicking a page item emits PageActivated for that page");
    }

    // Header intents.
    int addCount = 0;
    int settingsCount = 0;
    QObject::connect(&sidebar, &cnote::ui::SidebarWidget::AddPageRequested, [&]() { ++addCount; });
    QObject::connect(&sidebar, &cnote::ui::SidebarWidget::NewPageSettingsRequested, [&]() { ++settingsCount; });
    ClickWidget(sidebar, QPointF(sidebar.HeaderLayout().addPageRect.center()));
    ClickWidget(sidebar, QPointF(sidebar.HeaderLayout().newPageSettingsRect.center()));
    Check(addCount == 1, "add-page header click emits AddPageRequested");
    Check(settingsCount == 1, "new-page-settings header click emits NewPageSettingsRequested");

    // Current-page reveal scrolls the list so the highlighted page is visible.
    sidebar.SetScrollY(0);
    sidebar.SetCurrentPage(11);
    Check(WaitFor([&]() { return sidebar.ScrollY() > 0; }, 1000), "selecting an offscreen page scrolls the sidebar (90 ms animation)");
    const auto currentVisible = [&]() {
        const QRect content = sidebar.HeaderLayout().contentRect.toAlignedRect();
        const std::optional<QRect> item = sidebar.PageItemRect(11);
        return item.has_value() && item->bottom() >= content.top() && item->top() <= content.bottom();
    };
    Check(WaitFor(currentVisible, 1000), "the current page is visible after the scroll animation completes");
}

void TestSidebarWheelIsolation() {
    QWidget host;
    host.resize(1400, 900);
    cnote::ui::SidebarWidget sidebar(&host);
    const cnote::Document document = BuildDocument(12);
    sidebar.SetDocument(&document, nullptr);
    sidebar.SetHostClientRect(host.rect(), 34);
    sidebar.SetOpen(true);
    host.show();
    QApplication::processEvents();
    Check(WaitFor([&]() { return sidebar.SlideProgress() >= 0.999; }, 3000), "sidebar open before wheel test");

    QWidget canvasProbe(&host);
    canvasProbe.setGeometry(176, 34, 1200, 800);
    canvasProbe.show();

    int scrollSignals = 0;
    QObject::connect(&sidebar, &cnote::ui::SidebarWidget::ScrollChanged, [&](int) { ++scrollSignals; });

    // A wheel over the sidebar scrolls the sidebar locally and never reaches the
    // canvas sibling.
    class WheelCounter : public QObject {
    public:
        int count = 0;
    protected:
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Wheel) {
                ++count;
                return true;
            }
            return false;
        }
    };
    WheelCounter counter;
    canvasProbe.installEventFilter(&counter);

    const int scrollBefore = sidebar.ScrollY();
    SendWheel(sidebar, QPointF(88.0, 400.0), -120);
    Check(counter.count == 0, "a wheel over the sidebar does not reach the canvas sibling");
    Check(scrollSignals >= 1, "sidebar-local wheel emits ScrollChanged");
    Check(sidebar.ScrollY() > scrollBefore, "sidebar-local wheel scrolls the sidebar list");
    std::printf("INFO  sidebar wheel scrollY %d -> %d\n", scrollBefore, sidebar.ScrollY());

    const int scrollAfter = sidebar.ScrollY();
    SendWheel(canvasProbe, QPointF(400.0, 400.0), -120);
    Check(counter.count == 1, "a wheel over the canvas reaches the canvas");
    Check(sidebar.ScrollY() == scrollAfter, "canvas wheel does not move the sidebar list");

    // The list clamps at the bottom.
    sidebar.ScrollByPixels(100000);
    Check(sidebar.ScrollY() == sidebar.MaxScroll(), "sidebar scroll clamps at MaxScroll");
    sidebar.ScrollByPixels(-100000);
    Check(sidebar.ScrollY() == 0, "sidebar scroll clamps at zero");
}

void TestSidebarEnsureVisibleFromScrolled() {
    QWidget host;
    host.resize(1400, 900);
    cnote::ui::SidebarWidget sidebar(&host);
    const cnote::Document document = BuildDocument(12);
    sidebar.SetDocument(&document, nullptr);
    sidebar.SetHostClientRect(host.rect(), 34);
    sidebar.SetOpen(true);
    host.show();
    QApplication::processEvents();
    Check(WaitFor([&]() { return sidebar.SlideProgress() >= 0.999; }, 3000), "sidebar open before ensure-visible test");
    Check(sidebar.MaxScroll() > 0, "a 12-page document scrolls");

    const auto itemFullyVisible = [&](std::size_t index) {
        const QRect content = sidebar.HeaderLayout().contentRect.toAlignedRect();
        const std::optional<QRect> item = sidebar.PageItemRect(index);
        return item.has_value() && item->top() >= content.top() && item->bottom() <= content.bottom();
    };

    // Scroll to the bottom, then select a page that is already fully visible.
    // The old math compared widget-space item rect against scroll-space band
    // edges and jumped back toward the top; nothing should move here.
    sidebar.SetScrollY(sidebar.MaxScroll());
    QApplication::processEvents();
    Check(itemFullyVisible(10) || itemFullyVisible(11), "bottom pages are visible at max scroll");
    const int parked = sidebar.ScrollY();
    sidebar.SetCurrentPage(10);
    // Give a wrong ensure-visible request time to animate; the correct code
    // never starts one because the page is already inside the band.
    WaitFor([&]() { return false; }, 250);
    Check(sidebar.ScrollY() == parked, "selecting an already-visible page does not move the list");
    Check(itemFullyVisible(10), "the selected page stays fully visible");

    // A page below the fold lands at the bottom edge of the content band, not
    // scrollY_ pixels short of it.
    sidebar.SetScrollY(sidebar.MaxScroll() / 2);
    QApplication::processEvents();
    const int halfScroll = sidebar.ScrollY();
    sidebar.SetCurrentPage(11);
    Check(WaitFor([&]() {
        const QRect content = sidebar.HeaderLayout().contentRect.toAlignedRect();
        const std::optional<QRect> item = sidebar.PageItemRect(11);
        return item.has_value() && item->bottom() <= content.bottom();
    }, 1000), "selecting a page below the fold scrolls it into view");
    Check(sidebar.ScrollY() > halfScroll, "the list scrolled down for the new page");
    {
        const QRect content = sidebar.HeaderLayout().contentRect.toAlignedRect();
        const std::optional<QRect> item = sidebar.PageItemRect(11);
        Check(item.has_value() && item->bottom() <= content.bottom() + 1,
              "the revealed page sits inside the band, not past it");
    }

    // Symmetric case: a page above the fold lands at the top edge.
    sidebar.SetCurrentPage(0);
    Check(WaitFor([&]() {
        const QRect content = sidebar.HeaderLayout().contentRect.toAlignedRect();
        const std::optional<QRect> item = sidebar.PageItemRect(0);
        return item.has_value() && item->top() >= content.top() - 1;
    }, 1000), "selecting a page above the fold scrolls it to the top edge");
}

void TestToolbarAutoHide() {
    QWidget host;
    host.resize(1400, 900);
    cnote::ui::ToolbarWidget* toolbar = new cnote::ui::ToolbarWidget(&host);
    cnote::settings::Settings settings = BuildSettings();
    toolbar->SetSettings(settings);
    toolbar->resize(1400, 34);
    host.show();
    QApplication::processEvents();

    toolbar->SetAutoHideEnabled(true);
    Check(toolbar->AutoHideEnabled(), "auto-hide enabled");
    Check(!toolbar->Revealed(), "auto-hide starts hidden");
    CheckNear(toolbar->RevealProgress() <= 0.001, "auto-hide starts at zero progress", toolbar->RevealProgress(), 0.0, 0.001);

    const qreal toolbarHeight = qt::ToolbarHeight(false);
    Check(toolbar->ToolbarOffsetY() <= -toolbarHeight, "hidden toolbar is offset above the client area");

    // A pointer in the reference reveal band shows the toolbar.
    QElapsedTimer revealTimer;
    revealTimer.start();
    toolbar->UpdateAutoHideForCursor(QPointF(100.0, 4.0), false);
    Check(WaitFor([&]() { return toolbar->RevealProgress() >= 0.999; }, 2000), "reveal animation completes within the bound");
    const qint64 revealMs = revealTimer.elapsed();
    CheckNear(std::abs(toolbar->ToolbarOffsetY()) <= 0.5, "revealed toolbar sits at the client top", toolbar->ToolbarOffsetY(), 0.0, 0.5);
    Check(revealMs <= 900, "reveal uses an elapsed-time animation, not a blocking wait");
    std::printf("INFO  toolbar reveal observed=%lld ms (reference 120 ms)\n", static_cast<long long>(revealMs));

    // Leaving the toolbar band hides it again; an active interaction blocks it.
    toolbar->UpdateAutoHideForCursor(QPointF(100.0, 400.0), false);
    Check(WaitFor([&]() { return toolbar->RevealProgress() <= 0.001; }, 2000), "hide animation completes within the bound");
    Check(!toolbar->Revealed(), "toolbar hides after leaving the band");

    toolbar->SetRevealed(false);
    toolbar->UpdateAutoHideForCursor(QPointF(100.0, 4.0), false);
    Check(WaitFor([&]() { return toolbar->Revealed(); }, 2000), "reveal request while hidden is accepted");
    toolbar->UpdateAutoHideForCursor(QPointF(100.0, 400.0), true);
    Check(WaitFor([&]() { return toolbar->RevealProgress() >= 0.999; }, 2000), "an active interaction keeps the toolbar visible");
}

void TestOverlayDismissalConsumesPaletteMenu() {
    QWidget host;
    host.resize(1200, 800);
    cnote::ui::ToolbarWidget* toolbar = new cnote::ui::ToolbarWidget(&host);
    toolbar->SetSettings(BuildSettings());
    toolbar->resize(1200, 34);
    host.show();
    QApplication::processEvents();

    class WheelCounter : public QObject {
    public:
        int presses = 0;
    protected:
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::MouseButtonPress) {
                ++presses;
            }
            return false;
        }
    };
    QWidget probe(&host);
    probe.setGeometry(0, 400, 1200, 300);
    probe.show();
    WheelCounter counter;
    probe.installEventFilter(&counter);

    toolbar->TogglePaletteMenu();
    Check(toolbar->PaletteMenuVisible(), "palette menu opens");
    Check(host.rect().contains(toolbar->PaletteMenuBounds().toAlignedRect()), "palette menu stays inside the window");

    // A swatch click inside the open menu selects that palette index.  This
    // catches the panel being painted/hit-tested in host coordinates.
    std::optional<std::size_t> menuColor;
    QObject::connect(toolbar, &cnote::ui::ToolbarWidget::ColorSelected, [&](std::size_t index) { menuColor = index; });
    const qt::PaletteMenuLayout menuLayout = qt::BuildPaletteMenuLayout(toolbar->ComputeLayout(), toolbar->Settings().palette.size());
    if (QWidget* menu = host.findChild<QWidget*>(QStringLiteral("cnotePaletteMenu")); menu != nullptr && menu->isVisible()) {
        const QRect panel = menu->geometry();
        const QPoint swatchCenter = menuLayout.swatchRects[1].toAlignedRect().center() - panel.topLeft();
        ClickWidget(*menu, QPointF(swatchCenter));
        Check(menuColor.has_value() && *menuColor == 1U, "palette menu swatch click emits ColorSelected(1)");
    } else {
        Check(false, "palette menu overlay is a visible child of the host");
    }

    toolbar->TogglePaletteMenu();
    Check(toolbar->PaletteMenuVisible(), "palette menu reopens for the dismissal check");
    SendMouse(probe, QEvent::MouseButtonPress, QPointF(600.0, 500.0), Qt::LeftButton, Qt::LeftButton);
    Check(!toolbar->PaletteMenuVisible(), "palette menu closes on the first click outside it");
    Check(counter.presses == 0, "the closing click is consumed by the palette menu, like the reference");
}

// Every toolbar tool button draws its artwork from the compiled resource
// bundle; an icon missing from CMake's resource list silently falls back to
// a letter glyph (or to a stray filesystem copy).
void TestToolbarToolIconsAreBundled() {
    for (const char* name : { "lasso.svg", "cursor.svg", "text.svg", "latex.svg", "draw.svg", "pan.svg", "erase.svg", "zoom.svg" }) {
        const QString fileName = QString::fromLatin1(name);
        Check(cnote::ui::ChromeIconPath(fileName).startsWith(QStringLiteral(":/")),
            std::string("tool icon ") + name + " is in the compiled resources");
        Check(!cnote::ui::ChromeIconPixmap(fileName, QSize(30, 30), Qt::black).isNull(),
            std::string("tool icon ") + name + " renders");
    }
}

// Every bundled icon renders as its SVG describes: the chrome's own parser
// (no QtSvg in the app) is compared against Qt's SVG renderer at the same
// fit.  Catches styles the parser drops - e.g. fill/stroke set on the root
// <svg> instead of the <path>, which drew stroke-only icons as filled shapes.
void TestBundledIconsMatchSvgRenderer() {
    static_cast<void>(cnote::ui::ChromeIconPath(QStringLiteral("draw.svg")));  // registers the resource bundle
    const QString root = QStringLiteral(":/cnote/icons/");
    int compared = 0;
    QDirIterator icons(root, { QStringLiteral("*.svg") }, QDir::Files, QDirIterator::Subdirectories);
    while (icons.hasNext()) {
        const QString resource = icons.next();
        const QString name = resource.mid(root.size());
        constexpr int kSize = 64;
        const QImage ours = cnote::ui::ChromeIconPixmap(name, QSize(kSize, kSize), Qt::black).toImage()
            .convertToFormat(QImage::Format_ARGB32);
        QSvgRenderer renderer(resource);
        Check(!ours.isNull() && renderer.isValid(), "icon " + name.toStdString() + " loads in both renderers");
        if (ours.isNull() || !renderer.isValid()) {
            continue;
        }
        // Same fit as ChromeIconPixmap: viewBox into the size minus 8, centred.
        const QRectF box = renderer.viewBoxF();
        const qreal scale = std::min((kSize - 8.0) / box.width(), (kSize - 8.0) / box.height());
        const QRectF target((kSize - box.width() * scale) / 2.0, (kSize - box.height() * scale) / 2.0,
            box.width() * scale, box.height() * scale);
        QImage reference(kSize, kSize, QImage::Format_ARGB32);
        reference.fill(Qt::transparent);
        QPainter painter(&reference);
        painter.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&painter, target);
        painter.end();

        int referenceInk = 0;
        int mismatch = 0;
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const bool oursInk = qAlpha(ours.pixel(x, y)) > 128;
                const bool referenceHasInk = qAlpha(reference.pixel(x, y)) > 128;
                referenceInk += referenceHasInk ? 1 : 0;
                mismatch += oursInk != referenceHasInk ? 1 : 0;
            }
        }
        // Anti-aliasing differs by a few edge pixels; a dropped style differs
        // by a large share of the ink.
        Check(mismatch <= std::max(4, referenceInk / 50), "icon " + name.toStdString() + " matches its SVG ("
            + std::to_string(mismatch) + " of " + std::to_string(referenceInk) + " ink pixels differ)");
        ++compared;
    }
    Check(compared >= 18, "every bundled icon is compared (" + std::to_string(compared) + ")");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cnote_chrome_tests"));

    TestToolbarLayoutMatrix();
    TestToolbarIntentsAndHover();
    TestPaletteWheelIntents();
    TestPaletteWheelAnchor();
    TestPopupPlacementAndClamping();
    TestSidebarListAndNavigation();
    TestSidebarWheelIsolation();
    TestSidebarEnsureVisibleFromScrolled();
    TestToolbarAutoHide();
    TestOverlayDismissalConsumesPaletteMenu();
    TestToolbarToolIconsAreBundled();
    TestBundledIconsMatchSvgRenderer();

    std::printf(g_failures == 0 ? "ALL CHROME CHECKS PASSED\n" : "%d CHROME CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
