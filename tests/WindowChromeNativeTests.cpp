// Windows-only contract test for the native half of the borderless window
// chrome: QtMainWindow's WM_NCCALCSIZE / WM_NCACTIVATE / WM_NCHITTEST handling,
// the caption-button press cycle (App::HandleMessage, MessageLoop.cpp:85-154)
// and the window-level input that drives the toolbar auto-hide.
//
// window_chrome_qt covers the platform-neutral arithmetic offscreen; this test
// drives the real window procedure.  The window is kept off screen the way the
// capture mode does it (shell/CaptureDriver.h: Qt::WA_DontShowOnScreen on the
// native platform), so the HWND exists with its real styles and DPI but never
// appears: messages go straight to it with SendMessage, and the test asserts
// it stayed invisible and never took the foreground.
//
// Not covered, because each needs a visible window: maximize/minimize through
// the caption buttons (ShowWindow would show it) and the maximized hit test
// (IsZoomed).  ResolveWindowChromeHit's maximized branch is covered offscreen.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <QApplication>
#include <QByteArray>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QScreen>
#include <QTemporaryDir>
#include <QWindow>

#include <windows.h>

#include "qt/QtMainWindow.h"
#include "settings/SettingsStoreQt.h"
#include "shell/AppManager.h"
#include "shell/CaptureDriver.h"
#include "ui/PointerPositionQt.h"
#include "ui/ToolbarWidget.h"

namespace {

int gFailures = 0;

void Check(bool condition, const std::string& what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition) {
        ++gFailures;
    }
}

LPARAM PointParam(int x, int y) {
    return static_cast<LPARAM>((static_cast<DWORD>(static_cast<WORD>(y)) << 16) | static_cast<WORD>(x));
}

struct NativeWindow {
    cnote::qt::QtMainWindow& window;
    HWND hwnd = nullptr;
    cnote::ui::ToolbarWidget* toolbar = nullptr;

    // Client logical position to physical screen coordinates, the space a
    // WM_NCHITTEST lParam carries.
    [[nodiscard]] POINT ScreenPoint(const QPointF& client) const {
        const qreal ratio = window.devicePixelRatioF();
        POINT point { qRound(client.x() * ratio), qRound(client.y() * ratio) };
        ClientToScreen(hwnd, &point);
        return point;
    }

    [[nodiscard]] LRESULT HitTest(const POINT& screen) const {
        return SendMessageW(hwnd, WM_NCHITTEST, 0, PointParam(screen.x, screen.y));
    }

    [[nodiscard]] LRESULT HitTestClient(const QPointF& client) const {
        return HitTest(ScreenPoint(client));
    }

    // Centre of a toolbar control in window client coordinates.
    [[nodiscard]] QPointF ControlCenter(cnote::ui::ToolbarControl control) const {
        return QPointF(toolbar->mapTo(&window, toolbar->ControlRect(control).center().toPoint()));
    }

    // Client position as a client-area mouse message lParam (physical pixels).
    [[nodiscard]] LPARAM ClientParam(const QPointF& client) const {
        const qreal ratio = window.devicePixelRatioF();
        return PointParam(qRound(client.x() * ratio), qRound(client.y() * ratio));
    }
};

void TestNonClientArea(const NativeWindow& native) {
    RECT windowRect {};
    GetWindowRect(native.hwnd, &windowRect);

    // wParam TRUE carries the full parameter block, as during a real resize.
    WINDOWPOS position {};
    const auto calcParams = [&]() {
        NCCALCSIZE_PARAMS params {};
        params.rgrc[0] = windowRect;
        params.rgrc[1] = windowRect;
        params.rgrc[2] = windowRect;
        params.lppos = &position;
        return params;
    };
    NCCALCSIZE_PARAMS params = calcParams();
    const LRESULT calc = SendMessageW(native.hwnd, WM_NCCALCSIZE, TRUE, reinterpret_cast<LPARAM>(&params));
    Check(calc == 0 && EqualRect(&params.rgrc[0], &windowRect) != FALSE,
        "WM_NCCALCSIZE keeps the whole window rectangle as client area");

    // The default handler would reserve the caption and frame, so the check
    // above observes the window procedure, not a no-op.
    NCCALCSIZE_PARAMS defaults = calcParams();
    DefWindowProcW(native.hwnd, WM_NCCALCSIZE, TRUE, reinterpret_cast<LPARAM>(&defaults));
    Check(defaults.rgrc[0].top > windowRect.top, "the default WM_NCCALCSIZE would reserve a caption");

    Check(SendMessageW(native.hwnd, WM_NCACTIVATE, FALSE, 0) == TRUE,
        "WM_NCACTIVATE is answered without drawing a frame");
}

void TestHitCodes(const NativeWindow& native) {
    using cnote::ui::ToolbarControl;
    Check(native.HitTestClient(native.ControlCenter(ToolbarControl::Close)) == HTCLOSE, "close button -> HTCLOSE");
    Check(native.HitTestClient(native.ControlCenter(ToolbarControl::Maximize)) == HTMAXBUTTON,
        "maximize button -> HTMAXBUTTON");
    Check(native.HitTestClient(native.ControlCenter(ToolbarControl::Minimize)) == HTMINBUTTON,
        "minimize button -> HTMINBUTTON");
    const ToolbarControl drawTool = cnote::ui::ToolbarWidget::ControlForTool(cnote::settings::Tool::Draw);
    Check(native.HitTestClient(native.ControlCenter(drawTool)) == HTCLIENT, "a band tool control stays client (clickable)");

    // An empty stretch of the band drags the window.
    const qreal bandY = native.toolbar->VisibleToolbarHeight() / 2.0;
    QPointF empty(-1.0, bandY);
    for (qreal x = native.window.width() / 4.0; x < native.window.width() * 0.75; x += 4.0) {
        if (native.toolbar->ControlAtHostPosition(QPointF(x, bandY)) == ToolbarControl::None) {
            empty.setX(x);
            break;
        }
    }
    Check(empty.x() >= 0.0 && native.HitTestClient(empty) == HTCAPTION, "empty band -> HTCAPTION");

    const QPointF canvas(native.window.width() / 2.0, native.window.height() / 2.0);
    Check(native.HitTestClient(canvas) == HTCLIENT, "canvas -> HTCLIENT");

    // Frame zones, in screen pixels just inside the window rectangle.
    RECT rect {};
    GetWindowRect(native.hwnd, &rect);
    const LONG midX = (rect.left + rect.right) / 2;
    const LONG midY = (rect.top + rect.bottom) / 2;
    Check(native.HitTest(POINT { rect.left + 1, midY }) == HTLEFT, "left edge -> HTLEFT");
    Check(native.HitTest(POINT { rect.right - 2, midY }) == HTRIGHT, "right edge -> HTRIGHT");
    Check(native.HitTest(POINT { midX, rect.top + 1 }) == HTTOP, "top edge -> HTTOP");
    Check(native.HitTest(POINT { midX, rect.bottom - 2 }) == HTBOTTOM, "bottom edge -> HTBOTTOM");
    Check(native.HitTest(POINT { rect.left + 1, rect.top + 1 }) == HTTOPLEFT, "top-left corner -> HTTOPLEFT");
    Check(native.HitTest(POINT { rect.right - 2, rect.bottom - 2 }) == HTBOTTOMRIGHT,
        "bottom-right corner -> HTBOTTOMRIGHT");
}

void TestCaptionButtonCycle(const NativeWindow& native) {
    using cnote::ui::ToolbarControl;
    const QPointF close = native.ControlCenter(ToolbarControl::Close);
    const POINT closeScreen = native.ScreenPoint(close);
    const QPointF canvas(native.window.width() / 2.0, native.window.height() / 2.0);

    // Press arms the button; releasing elsewhere cancels it.
    SendMessageW(native.hwnd, WM_NCLBUTTONDOWN, HTCLOSE, PointParam(closeScreen.x, closeScreen.y));
    Check(native.toolbar->PressedControl() == ToolbarControl::Close, "WM_NCLBUTTONDOWN on close arms the button");
    SendMessageW(native.hwnd, WM_LBUTTONUP, 0, native.ClientParam(canvas));
    QApplication::processEvents();
    Check(native.toolbar->PressedControl() == ToolbarControl::None && native.window.isVisible(),
        "releasing off the button cancels the close");

    // Press and release on the button closes the window.
    SendMessageW(native.hwnd, WM_NCLBUTTONDOWN, HTCLOSE, PointParam(closeScreen.x, closeScreen.y));
    SendMessageW(native.hwnd, WM_LBUTTONUP, 0, native.ClientParam(close));
    QApplication::processEvents();
    Check(!native.window.isVisible(), "press and release on close closes the window");
}

// Toolbar auto-hide input at the window level: the cursor crossing the client
// boundary, leaving through the non-client band, and a canvas drag reaching the
// reveal strip.  `native` is a window whose settings enable auto-hide.
void TestAutoHideInput(const NativeWindow& native) {
    cnote::ui::ToolbarWidget& toolbar = *native.toolbar;
    QWindow* handle = native.window.windowHandle();
    const qreal midX = native.window.width() / 2.0;
    const QPointF belowStrip(midX, 20.0);  // in the band area, below the 12-pixel reveal strip
    const QPointF aboveWindow(midX, -20.0);
    const QPointF canvas(midX, native.window.height() / 2.0);
    const auto global = [&](const QPointF& client) { return native.window.mapToGlobal(client); };
    const auto leaveTo = [&](const QPointF& client) {
        cnote::ui::SetSyntheticCursorPos(global(client).toPoint());
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(handle, &leave);
    };
    const auto ncLeaveTo = [&](const QPointF& client) {
        cnote::ui::SetSyntheticCursorPos(global(client).toPoint());
        SendMessageW(native.hwnd, WM_NCMOUSELEAVE, 0, 0);
    };
    const auto mouse = [&](QEvent::Type type, const QPointF& client, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, client, global(client), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &event);
    };

    Check(toolbar.AutoHideEnabled() && !toolbar.Revealed(), "an auto-hide window starts with the band hidden");

    // Qt turns the first client move after entering into an Enter event only,
    // so entering through the band must reveal on its own.
    QEnterEvent enter(belowStrip, belowStrip, global(belowStrip));
    QCoreApplication::sendEvent(handle, &enter);
    Check(toolbar.Revealed(), "entering the window inside the band area reveals the band");

    leaveTo(aboveWindow);
    Check(!toolbar.Revealed(), "leaving the client area for outside the window hides the band");

    Check(native.HitTestClient(QPointF(midX, 4.0)) == HTTOP && toolbar.Revealed(),
        "the top frame strip reveals the band");
    ncLeaveTo(belowStrip);
    Check(toolbar.Revealed(), "a non-client leave that stays inside the band keeps it");
    ncLeaveTo(aboveWindow);
    Check(!toolbar.Revealed(), "leaving the window from the non-client band hides it");

    // A stroke dragged into the reveal strip must not flash the band.
    mouse(QEvent::MouseButtonPress, canvas, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, QPointF(midX, 5.0), Qt::NoButton, Qt::LeftButton);
    Check(!toolbar.Revealed(), "a canvas drag into the reveal strip keeps the band hidden");
    mouse(QEvent::MouseButtonRelease, QPointF(midX, 5.0), Qt::LeftButton, Qt::NoButton);
    mouse(QEvent::MouseMove, QPointF(midX, 5.0), Qt::NoButton, Qt::NoButton);
    Check(toolbar.Revealed(), "the reveal strip works again once the drag ends");

    cnote::ui::SetSyntheticCursorPos(std::nullopt);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cnote"));
    // The window loads and may write settings: keep the developer's file out of it.
    QTemporaryDir settings;
    cnote::settings::SettingsStoreQt::OverrideSettingsPath(
        std::filesystem::path(settings.path().toStdWString()) / "settings.ini");
    cnote::qt::QtMainWindow::SetHeadlessModals(true);

    cnote::shell::OffscreenWindows offscreen;
    app.installEventFilter(&offscreen);
    // Checked as "not ours" rather than "unchanged": the user may switch
    // applications while the test runs.
    const auto ownsForeground = []() {
        DWORD owner = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &owner);
        return owner == GetCurrentProcessId();
    };
    const auto withWindow = [&](const std::function<void(const NativeWindow&)>& body) {
        cnote::AppManager manager;
        cnote::qt::QtMainWindow window(manager);
        offscreen.SetPrimaryWindow(&window);
        // Inside the work area: Windows clamps a window larger than the screen
        // (as on small CI runner screens), which would cut the caption cluster
        // off the native window while the widget still lays it out.
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        window.move(available.topLeft() + QPoint(20, 20));
        window.show();
        window.resize(std::min(1000, available.width() - 60), std::min(700, available.height() - 60));
        QApplication::processEvents();

        NativeWindow native { window, reinterpret_cast<HWND>(window.winId()),
            window.findChild<cnote::ui::ToolbarWidget*>() };
        Check(native.toolbar != nullptr, "the window has its toolbar band");
        if (native.toolbar != nullptr) {
            Check(IsWindowVisible(native.hwnd) == FALSE, "the native window exists but is not visible");
            body(native);
            Check(IsWindowVisible(native.hwnd) == FALSE, "the native window never became visible");
            Check(!ownsForeground(), "the window never took the foreground");
        }
    };
    withWindow([](const NativeWindow& native) {
        TestNonClientArea(native);
        TestHitCodes(native);
        TestCaptionButtonCycle(native);
    });
    // Auto-hide comes from the saved settings, applied as the window is built,
    // exactly as on a launch.
    {
        const cnote::settings::SettingsStoreQt store;
        cnote::settings::Settings saved = store.Load();
        saved.autoHideToolbar = true;
        Check(store.Save(saved), "the auto-hide setting is saved");
    }
    withWindow(TestAutoHideInput);
    Check(!ownsForeground(), "the process does not own the foreground window");

    std::printf(gFailures == 0 ? "ALL NATIVE WINDOW CHROME CHECKS PASSED\n" : "%d NATIVE WINDOW CHROME CHECK(S) FAILED\n",
        gFailures);
    return gFailures == 0 ? 0 : 1;
}
