// Deterministic offscreen contract test for the reference window chrome
// (migration plan step 6.3, superseded by the user requirement that the native
// title bar must not be visible).
//
// The Windows shell removes the caption through the native window handle
// (WM_NCCALCSIZE / WM_NCACTIVATE / WM_NCHITTEST, App::HandleMessage
// MessageLoop.cpp:85-154) and lets the toolbar band carry the app's own
// minimize/maximize/close cluster.  This test covers the half of that contract
// that is platform-neutral and therefore testable offscreen:
//
//   * ResolveWindowChromeHit: the DPI-scaled frame zones and the band's
//     caption/client split the window procedure maps onto the HT* codes.
//   * band/control geometry under the native frame inset, both window states.
//   * the maximize/restore glyph transition and the band height transition.
//   * the auto-hide band visibility state machine (App::VisibleToolbarHeight).
//   * the caption-button command intents and the host-coordinate hit test the
//     native WM_NCHITTEST shares with the widget's own mouse routing.
//
// Every wait is bounded, so a regression can never hang the suite.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QImage>
#include <QMouseEvent>
#include <QPixmap>
#include <QWidget>

#include "settings/SettingsStoreQt.h"
#include "ui/ChromeStyleQt.h"
#include "ui/LayoutQt.h"
#include "ui/ToolbarWidget.h"
#include "ui/WindowBehaviorQt.h"

namespace {

namespace qt = cnote::appui::qt;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void CheckNear(double actual, double expected, double tolerance, const std::string& what) {
    if (std::fabs(actual - expected) > tolerance) {
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

bool WaitFor(const std::function<bool()>& predicate, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return predicate();
}

cnote::settings::Settings BuildSettings() {
    cnote::settings::Settings settings;
    settings.palette = cnote::settings::DefaultPalette();
    settings.colorIndex = 2;
    settings.drawBrushSize = 5.0f;
    settings.quickSizeSliderMax = 20.0f;
    return settings;
}

// The physical frame metrics measured on the 175% reference display
// (SM_CXFRAME + SM_CXPADDEDBORDER at 168 DPI, MessageLoop.cpp:68-70).
constexpr int kReferenceFrameX = 12;
constexpr int kReferenceFrameY = 12;

// A pixel counts as lit when it is clearly brighter than the band background
// (15, 18, 24) even after the 1-px glyph pen is antialiased.
bool Lit(const QImage& image, int x, int y) {
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) {
        return false;
    }
    const QColor color = image.pixelColor(x, y);
    return (color.red() + color.green() + color.blue()) > 240;
}

// Bounding box of the lit pixels inside `rect`, in image coordinates.
QRect LitBounds(const QImage& image, const QRect& rect) {
    int left = rect.right();
    int top = rect.bottom();
    int right = rect.left() - 1;
    int bottom = rect.top() - 1;
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        for (int x = rect.left(); x <= rect.right(); ++x) {
            if (Lit(image, x, y)) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < left || bottom < top) {
        return {};
    }
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

bool Reddish(const QColor& color) {
    return color.red() > color.green() + 40 && color.red() > color.blue() + 40;
}

bool Greenish(const QColor& color) {
    return color.green() > color.red() + 40 && color.green() > color.blue() + 40;
}

// --- native hit-test arithmetic ----------------------------------------------

void TestWindowChromeHitArithmetic() {
    cnote::ui::WindowChromeHitTest hit;
    hit.windowRect = QRect(300, 200, 1700, 1100);
    hit.frameThicknessX = kReferenceFrameX;
    hit.frameThicknessY = kReferenceFrameY;

    const auto at = [&](int x, int y) {
        hit.cursor = QPoint(x, y);
        return cnote::ui::ResolveWindowChromeHit(hit);
    };

    Check(at(1000, 700) == cnote::ui::WindowChromeHit::Client, "client body is HTCLIENT");
    Check(at(1000, 202) == cnote::ui::WindowChromeHit::Top, "top frame strip is HTTOP");
    Check(at(1000, 1297) == cnote::ui::WindowChromeHit::Bottom, "bottom frame strip is HTBOTTOM");
    Check(at(302, 700) == cnote::ui::WindowChromeHit::Left, "left frame strip is HTLEFT");
    Check(at(1997, 700) == cnote::ui::WindowChromeHit::Right, "right frame strip is HTRIGHT");
    Check(at(302, 202) == cnote::ui::WindowChromeHit::TopLeft, "top-left corner is HTTOPLEFT");
    Check(at(1997, 202) == cnote::ui::WindowChromeHit::TopRight, "top-right corner is HTTOPRIGHT");
    Check(at(302, 1297) == cnote::ui::WindowChromeHit::BottomLeft, "bottom-left corner is HTBOTTOMLEFT");
    Check(at(1997, 1297) == cnote::ui::WindowChromeHit::BottomRight, "bottom-right corner is HTBOTTOMRIGHT");
    // Exclusive frame edges: one pixel past the inset is client again.
    Check(at(312, 700) == cnote::ui::WindowChromeHit::Client, "one pixel inside the left frame is client");
    Check(at(1000, 212) == cnote::ui::WindowChromeHit::Client, "one pixel inside the top frame is client");
    Check(at(1999 - kReferenceFrameX, 700) == cnote::ui::WindowChromeHit::Client, "one pixel inside the right frame is client");

    // The band splits into controls (client) and a caption drag strip.
    hit.cursorInBand = true;
    hit.cursorOverToolbarControl = false;
    hit.cursor = QPoint(1000, 300);
    Check(cnote::ui::ResolveWindowChromeHit(hit) == cnote::ui::WindowChromeHit::Caption, "band strip is HTCAPTION");
    hit.cursorOverToolbarControl = true;
    Check(cnote::ui::ResolveWindowChromeHit(hit) == cnote::ui::WindowChromeHit::Client, "band control is HTCLIENT");
    // The in-client caption cluster reports the real non-client codes, so the
    // system runs the press/activate cycle (and the Windows 11 snap-layouts
    // flyout) while the widget only paints the state.
    hit.cursorOverMinButton = true;
    Check(cnote::ui::ResolveWindowChromeHit(hit) == cnote::ui::WindowChromeHit::MinButton, "minimize button is HTMINBUTTON");
    hit.cursorOverMinButton = false;
    hit.cursorOverMaxButton = true;
    Check(cnote::ui::ResolveWindowChromeHit(hit) == cnote::ui::WindowChromeHit::MaxButton, "maximize button is HTMAXBUTTON");
    hit.cursorOverMaxButton = false;
    hit.cursorOverCloseButton = true;
    Check(cnote::ui::ResolveWindowChromeHit(hit) == cnote::ui::WindowChromeHit::CloseButton, "close button is HTCLOSE");
    hit.cursorOverCloseButton = false;

    // A maximized window has no resize zones: the frame strips fall through to
    // the band/client decision (MessageLoop.cpp:90-154).
    hit.maximized = true;
    hit.cursorOverToolbarControl = false;
    hit.cursorInBand = false;
    Check(at(1000, 202) == cnote::ui::WindowChromeHit::Client, "maximized top strip is not a resize zone");
    Check(at(302, 202) == cnote::ui::WindowChromeHit::Client, "maximized corner is not a resize zone");
    hit.cursorInBand = true;
    Check(at(1000, 202) == cnote::ui::WindowChromeHit::Caption, "maximized band strip drags the window");
}

// --- band geometry under the native frame inset -------------------------------

void TestBandGeometryAndFrameInset() {
    QWidget host;
    host.resize(1400, 34);
    cnote::ui::ToolbarWidget toolbar(&host);
    toolbar.SetSettings(BuildSettings());
    toolbar.SetCaptionButtonsVisible(true);
    toolbar.SetTitle(QStringLiteral("representative.cnote"));
    toolbar.SetZoomLabel(QStringLiteral("100%"));

    const qreal frameInset = 12.0 / 1.75;  // 12 physical px at the 175% reference display

    // Restored: the band is 34 logical pixels, the cluster ends at the client edge.
    toolbar.SetMaximized(false);
    host.resize(1400, 34);
    toolbar.resize(1400, 34);
    qt::ToolbarLayout layout = toolbar.ComputeLayout();
    CheckNear(toolbar.height(), 34.0, 0.001, "restored band height is ToolbarHeight(false)");
    CheckNear(layout.closeRect.right(), 1400.0, 0.001, "restored close button ends at the client right edge");
    CheckNear(layout.closeRect.top(), 0.0, 0.001, "restored caption cluster starts at band top");
    CheckNear(layout.closeRect.width(), 46.0, 0.001, "caption button width is the native 46");
    CheckNear(layout.maximizeRect.right(), layout.closeRect.left(), 0.001, "maximize rect abuts close");
    CheckNear(layout.minimizeRect.right(), layout.maximizeRect.left(), 0.001, "minimize rect abuts maximize");

    // Maximized: band 39, cluster inset by the frame width, cluster lowered by
    // the LayoutQt 5-pixel top inset.
    toolbar.SetFrameThicknessX(frameInset);
    toolbar.SetMaximized(true);
    host.resize(1400, 39);
    toolbar.resize(1400, 39);
    layout = toolbar.ComputeLayout();
    CheckNear(toolbar.height(), 39.0, 0.001, "maximized band height is ToolbarHeight(true)");
    CheckNear(layout.closeRect.right(), 1400.0 - frameInset, 0.001, "maximized close button is inset by the native frame");
    CheckNear(layout.closeRect.top(), 5.0, 0.001, "maximized caption cluster uses the ToolbarTopInset(5)");
    CheckNear(layout.closeRect.bottom(), 39.0, 0.001, "maximized caption cluster reaches the band bottom");
    CheckNear(layout.minimizeRect.right() - layout.minimizeRect.left(), 46.0, 0.001, "maximized minimize width");

    // Host-coordinate hit tests agree with the widget's own mouse routing, and
    // the widget's position (which carries the auto-hide reveal offset) is
    // included: this is the function the shell's WM_NCHITTEST calls.
    const QRectF close = toolbar.ControlRect(cnote::ui::ToolbarControl::Close);
    Check(toolbar.ControlAtHostPosition(QPointF(toolbar.x() + close.center().x(), toolbar.y() + close.center().y()))
              == cnote::ui::ToolbarControl::Close,
        "host hit test resolves the close button");
    Check(toolbar.ControlAtHostPosition(QPointF(toolbar.x() + 20.0, toolbar.y() + 20.0)) == cnote::ui::ToolbarControl::TitleMenu,
        "host hit test resolves the title menu");
    Check(toolbar.ControlAtHostPosition(QPointF(toolbar.x() + 20.0, toolbar.y() + toolbar.height() + 40.0))
              == cnote::ui::ToolbarControl::None,
        "host hit test rejects points below the band");
    Check(toolbar.ControlAtHostPosition(QPointF(700.0, 20.0)) == cnote::ui::ToolbarControl::None,
        "host hit test rejects the caption strip between title and tools");

    // Hover driven from the native hit test (the shell path) matches the widget's
    // own hover: it sets the control under the cursor and clears elsewhere.
    toolbar.UpdateHoverAtHostPosition(QPointF(toolbar.x() + close.center().x(), toolbar.y() + close.center().y()));
    Check(toolbar.HoveredControl() == cnote::ui::ToolbarControl::Close, "native hover sets close");
    toolbar.UpdateHoverAtHostPosition(QPointF(toolbar.x() + 700.0, toolbar.y() + 20.0));
    Check(toolbar.HoveredControl() == cnote::ui::ToolbarControl::None, "native hover clears on the caption strip");
}

void TestCaptionClusterHiddenWithoutCustomChrome() {
    // Platforms that keep native decorations call SetCaptionButtonsVisible(false);
    // the cluster must then neither paint nor answer a hit test, so a click on
    // its reserved area cannot emit a window command.
    QWidget host;
    host.resize(1400, 34);
    cnote::ui::ToolbarWidget toolbar(&host);
    toolbar.SetSettings(BuildSettings());
    toolbar.SetCaptionButtonsVisible(false);
    toolbar.SetMaximized(false);
    toolbar.resize(1400, 34);

    const QImage image = toolbar.grab().toImage();
    const QRect maximizeRect = toolbar.ControlRect(cnote::ui::ToolbarControl::Maximize).toAlignedRect();
    const qreal hiddenDpr = image.devicePixelRatio();
    Check(!Lit(image, static_cast<int>(maximizeRect.center().x() * hiddenDpr),
            static_cast<int>(maximizeRect.center().y() * hiddenDpr)), "hidden caption cluster is not painted");
    Check(toolbar.ControlAt(QPointF(maximizeRect.center())) == cnote::ui::ToolbarControl::None,
        "hidden caption cluster is not hit-testable");
}

// --- maximize/restore glyph and band height ----------------------------------

void TestMaximizeGlyphTransition() {
    QWidget host;
    host.resize(1400, 34);
    cnote::ui::ToolbarWidget toolbar(&host);
    toolbar.SetSettings(BuildSettings());
    toolbar.SetCaptionButtonsVisible(true);
    toolbar.SetFrameThicknessX(12.0 / 1.75);

    toolbar.SetMaximized(false);
    toolbar.resize(1400, 34);
    const QImage restored = toolbar.grab().toImage();
    // grab() returns a physical-pixel image (devicePixelRatio 1.75 on the
    // reference display); every rect it is indexed with must be scaled.
    const qreal imgDpr = restored.devicePixelRatio();
    const auto physical = [imgDpr](const QRect& logical) {
        return QRect(
            static_cast<int>(logical.x() * imgDpr),
            static_cast<int>(logical.y() * imgDpr),
            static_cast<int>(logical.width() * imgDpr),
            static_cast<int>(logical.height() * imgDpr));
    };
    const auto physicalPoint = [imgDpr](const QRect& logical) {
        return QPoint(
            static_cast<int>(logical.x() * imgDpr) + 4,
            static_cast<int>(logical.y() * imgDpr) + 4);
    };
    const QRect restoredRect = toolbar.ControlRect(cnote::ui::ToolbarControl::Maximize).toAlignedRect();
    const QRect restoredBounds = LitBounds(restored, physical(restoredRect));

    toolbar.SetMaximized(true);
    toolbar.resize(1400, 39);
    const QImage maximized = toolbar.grab().toImage();
    const QRect maximizedRect = toolbar.ControlRect(cnote::ui::ToolbarControl::Maximize).toAlignedRect();
    const QRect maximizedBounds = LitBounds(maximized, physical(maximizedRect));

    Check(restoredBounds.isValid(), "restored maximize glyph is painted");
    Check(maximizedBounds.isValid(), "maximized maximize glyph is painted");
    // The cluster draws the system's caption glyphs (Segoe Fluent/MDL2 chrome
    // icons): the maximized state must switch to the restore glyph, verified
    // as a different raster inside the same centred crop rather than by
    // pinning the glyph's drawn geometry.
    const QRect crop(
        physical(restoredRect).center().x() - 9, physical(restoredRect).center().y() - 9, 19, 19);
    const QImage restoredGlyph = restored.copy(crop);
    const QImage maximizedGlyph = maximized.copy(
        crop.translated(physical(maximizedRect).center() - physical(restoredRect).center()));
    Check(restoredGlyph != maximizedGlyph, "maximize glyph switches to the restore glyph when maximized");

    // Native caption state fills: hover on close paints the system red over
    // the whole button, and the pressed state darkens it (WinUI
    // shellCaptionCloseFill*).  Sampled off-centre so the glyph itself cannot
    // pollute the fill check.
    const QRect closeRect = toolbar.ControlRect(cnote::ui::ToolbarControl::Close).toAlignedRect();
    const QPoint fillPoint = physicalPoint(closeRect);
    const QColor restFill = QImage(toolbar.grab().toImage()).pixelColor(fillPoint);
    Check(!Reddish(restFill), "resting close button does not paint the hover red");
    toolbar.UpdateHoverAtHostPosition(QPointF(toolbar.x() + closeRect.center().x(), toolbar.y() + closeRect.center().y()));
    const QColor hoverFill = QImage(toolbar.grab().toImage()).pixelColor(fillPoint);
    Check(Reddish(hoverFill) && hoverFill.red() > 150, "close hover paints the system red");
    toolbar.SetPressedControl(cnote::ui::ToolbarControl::Close);
    const QColor pressedFill = QImage(toolbar.grab().toImage()).pixelColor(fillPoint);
    Check(Reddish(pressedFill), "close press keeps the red family");
    Check(pressedFill.red() < hoverFill.red() || pressedFill.alpha() < hoverFill.alpha(),
        "close press is dimmer than close hover");
    toolbar.SetPressedControl(cnote::ui::ToolbarControl::None);

    // The min/max hover is a subtle light overlay, not a saturated colour.
    const QRect minRect = toolbar.ControlRect(cnote::ui::ToolbarControl::Minimize).toAlignedRect();
    const QPoint minFillPoint = physicalPoint(minRect);
    const QColor minRestFill = QImage(toolbar.grab().toImage()).pixelColor(minFillPoint);
    toolbar.UpdateHoverAtHostPosition(QPointF(toolbar.x() + minRect.center().x(), toolbar.y() + minRect.center().y()));
    const QColor minHoverFill = QImage(toolbar.grab().toImage()).pixelColor(minFillPoint);
    Check(!Reddish(minHoverFill) && !Greenish(minHoverFill),
        "minimize hover does not paint a saturated colour");
    Check(std::abs(minHoverFill.red() - minRestFill.red()) > 3
            || std::abs(minHoverFill.green() - minRestFill.green()) > 3
            || std::abs(minHoverFill.blue() - minRestFill.blue()) > 3,
        "minimize hover changes the fill");
    toolbar.UpdateHoverAtHostPosition(QPointF(toolbar.x() + 700.0, toolbar.y() + 20.0));

    // The band transition itself: 34 -> 39 logical pixels.
    toolbar.SetMaximized(false);
    CheckNear(qt::ToolbarHeight(toolbar.Maximized()), 34.0, 0.001, "band height follows Maximized(false)");
    toolbar.SetMaximized(true);
    CheckNear(qt::ToolbarHeight(toolbar.Maximized()), 39.0, 0.001, "band height follows Maximized(true)");
}

// --- auto-hide band visibility ------------------------------------------------

void TestAutoHideBandStateMachine() {
    QWidget host;
    host.resize(1400, 34);
    cnote::ui::ToolbarWidget toolbar(&host);
    toolbar.SetSettings(BuildSettings());
    toolbar.SetCaptionButtonsVisible(true);

    CheckNear(toolbar.VisibleToolbarHeight(), 34.0, 0.001, "band is visible while auto-hide is off");
    CheckNear(toolbar.ToolbarOffsetY(), 0.0, 0.001, "band has no offset while auto-hide is off");

    toolbar.SetAutoHideEnabled(true);
    CheckNear(toolbar.VisibleToolbarHeight(), 0.0, 0.001, "enabling auto-hide hides the band");
    Check(!toolbar.Revealed(), "enabling auto-hide leaves the band hidden");
    Check(toolbar.pos().y() < 0, "hidden band is parked above the host");
    CheckNear(toolbar.ToolbarOffsetY(), -36.0, 0.001, "hidden band offset is -(band + 2)");
    // The whole band, caption cluster included, is off the host: the window
    // buttons cannot be clicked while the band is hidden.
    Check(toolbar.ControlAtHostPosition(QPointF(toolbar.ControlRect(cnote::ui::ToolbarControl::Close).center().x(), 10.0))
              == cnote::ui::ToolbarControl::None,
        "hidden band answers no control under the top edge");

    // Top-edge hover reveals (reference reveal band: 0..12 logical unmaximized).
    toolbar.UpdateAutoHideForCursor(QPointF(700.0, 5.0), false);
    Check(WaitFor([&]() { return toolbar.VisibleToolbarHeight() > 0.0 && toolbar.ToolbarOffsetY() == 0.0; }, 1000),
        "top-edge hover reveals the band");
    Check(toolbar.Revealed(), "revealed state is reported");

    // A cursor over the canvas hides it again.
    toolbar.UpdateAutoHideForCursor(QPointF(700.0, 600.0), false);
    Check(WaitFor([&]() { return toolbar.VisibleToolbarHeight() == 0.0; }, 1000), "leaving the band hides it again");

    // The reference guard list keeps the band up while an inline editor owns it.
    toolbar.BeginZoomEdit();
    CheckNear(toolbar.VisibleToolbarHeight(), 34.0, 0.001, "zoom editing keeps the band visible");
    toolbar.CancelZoomEdit();
    CheckNear(toolbar.VisibleToolbarHeight(), 0.0, 0.001, "cancelling the zoom edit hides it again");

    // An active drag (interaction guard) must not flash the band.
    toolbar.UpdateAutoHideForCursor(QPointF(700.0, 5.0), true);
    Check(!toolbar.Revealed(), "an active interaction does not reveal the band");

    // Turning auto-hide off restores a permanently visible band.
    toolbar.SetAutoHideEnabled(false);
    CheckNear(toolbar.VisibleToolbarHeight(), 34.0, 0.001, "disabling auto-hide shows the band");
    toolbar.UpdateAutoHideForCursor(QPointF(700.0, 900.0), false);
    CheckNear(toolbar.VisibleToolbarHeight(), 34.0, 0.001, "band stays visible with auto-hide off");
    CheckNear(toolbar.ToolbarOffsetY(), 0.0, 0.001, "band offset is zero with auto-hide off");
}

// --- caption-button intents ---------------------------------------------------

void TestCaptionButtonIntents() {
    QWidget host;
    host.resize(1400, 34);
    cnote::ui::ToolbarWidget toolbar(&host);
    toolbar.SetSettings(BuildSettings());
    toolbar.SetCaptionButtonsVisible(true);
    toolbar.SetMaximized(false);
    toolbar.resize(1400, 34);

    std::vector<cnote::ui::ToolbarCommand> commands;
    QObject::connect(&toolbar, &cnote::ui::ToolbarWidget::CommandTriggered, [&](cnote::ui::ToolbarCommand command) {
        commands.push_back(command);
    });

    const struct {
        cnote::ui::ToolbarControl control;
        cnote::ui::ToolbarCommand command;
        const char* name;
    } cases[] = {
        { cnote::ui::ToolbarControl::Minimize, cnote::ui::ToolbarCommand::Minimize, "minimize" },
        { cnote::ui::ToolbarControl::Maximize, cnote::ui::ToolbarCommand::Maximize, "maximize" },
        { cnote::ui::ToolbarControl::Close, cnote::ui::ToolbarCommand::Close, "close" },
        { cnote::ui::ToolbarControl::TitleMenu, cnote::ui::ToolbarCommand::OpenTitleMenu, "title menu" },
    };
    for (const auto& testCase : cases) {
        commands.clear();
        ClickWidget(toolbar, toolbar.ControlRect(testCase.control).center());
        Check(commands.size() == 1 && commands.front() == testCase.command,
            std::string("clicking the ") + testCase.name + " control emits its command");
    }

    // The native press cycle: a press arms the button without firing, the
    // command lands on a same-button release, and a release that slid off the
    // button cancels like the system caption.
    commands.clear();
    const QRectF closeRect = toolbar.ControlRect(cnote::ui::ToolbarControl::Close);
    SendMouse(toolbar, QEvent::MouseButtonPress, closeRect.center(), Qt::LeftButton, Qt::LeftButton);
    Check(toolbar.PressedControl() == cnote::ui::ToolbarControl::Close, "press arms the close button");
    Check(commands.empty(), "press alone does not emit the command");
    SendMouse(toolbar, QEvent::MouseButtonRelease, closeRect.center(), Qt::LeftButton, Qt::NoButton);
    Check(commands.size() == 1 && commands.front() == cnote::ui::ToolbarCommand::Close,
        "release on the armed button emits its command");

    commands.clear();
    const QRectF minimizeRect = toolbar.ControlRect(cnote::ui::ToolbarControl::Minimize);
    SendMouse(toolbar, QEvent::MouseButtonPress, minimizeRect.center(), Qt::LeftButton, Qt::LeftButton);
    SendMouse(toolbar, QEvent::MouseButtonRelease, QPointF(minimizeRect.center().x() - 200.0, 20.0), Qt::LeftButton, Qt::NoButton);
    Check(commands.empty(), "release off the armed button cancels the command");

    // A click on the band's caption strip belongs to the window (drag/snap), so
    // the widget must not consume it.
    commands.clear();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(700.0, 20.0), toolbar.mapToGlobal(QPoint(700, 20)),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&toolbar, &press);
    Check(!press.isAccepted() && commands.empty(), "clicking the caption strip is left to the window");

    // Hidden cluster: a click on the reserved minimize area must not reach it.
    QWidget plainHost;
    plainHost.resize(1400, 34);
    cnote::ui::ToolbarWidget plain(&plainHost);
    plain.SetSettings(BuildSettings());
    plain.SetCaptionButtonsVisible(false);
    plain.resize(1400, 34);
    std::vector<cnote::ui::ToolbarCommand> plainCommands;
    QObject::connect(&plain, &cnote::ui::ToolbarWidget::CommandTriggered, [&](cnote::ui::ToolbarCommand command) {
        plainCommands.push_back(command);
    });
    const QRectF minimized = plain.ControlRect(cnote::ui::ToolbarControl::Minimize);
    ClickWidget(plain, QPointF(minimized.center().x(), 20.0));
    Check(plainCommands.empty(), "no window command without the in-client caption cluster");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cnote_window_chrome_tests"));

    TestWindowChromeHitArithmetic();
    TestBandGeometryAndFrameInset();
    TestCaptionClusterHiddenWithoutCustomChrome();
    TestMaximizeGlyphTransition();
    TestAutoHideBandStateMachine();
    TestCaptionButtonIntents();

    std::printf(g_failures == 0 ? "ALL WINDOW CHROME CHECKS PASSED\n" : "%d WINDOW CHROME CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
