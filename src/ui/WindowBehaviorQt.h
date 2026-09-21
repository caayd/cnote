#pragma once

#include <algorithm>

#include <QPoint>
#include <QRect>
#include <QSize>

namespace cnote::ui {

// Window behavior for the Qt shell (migration plan step 6.3).
//
// The reference application starts at a 1400x960 logical client size
// (README/plan step 6.3) and stays resizable.
//
// Usage from the Qt window (do not add a second window class):
//
//   void MainWindow::showEvent(QShowEvent* event) {
//       QMainWindow::showEvent(event);
//       if (QScreen* screen = this->screen()) {
//           resize(cnote::ui::PreferredStartSize(screen->availableGeometry()));
//       }
//   }
//
// `availableGeometry()` already excludes taskbars/docks, so the clamp keeps the
// reference start size on small or fractionally-scaled displays instead of
// letting the window grow past the usable desktop.
inline QSize PreferredStartSize(const QRect& availableGeometry) {
    constexpr int kReferenceWidth = 1400;
    constexpr int kReferenceHeight = 960;
    return QSize(
        std::min(kReferenceWidth, std::max(1, availableGeometry.width())),
        std::min(kReferenceHeight, std::max(1, availableGeometry.height())));
}

// --- Windows custom caption hit testing -------------------------------------
//
// The reference frontend shows no native title bar: its window procedure reports
// the whole window rectangle as client area (App::HandleMessage WM_NCCALCSIZE,
// MessageLoop.cpp:87) and answers WM_NCHITTEST itself (MessageLoop.cpp:89-154).
// The Qt shell keeps that contract *and* keeps the WS_OVERLAPPEDWINDOW style -
// only the caption stops being drawn - because the style is what preserves the
// DWM shadow, Aero snap, double-click maximize/restore, the resize edges and the
// work-area maximize clamp; Qt::FramelessWindowHint gives all of those up.
//
// The arithmetic below is platform-neutral (no <windows.h>) so the offscreen
// CTest can drive it; the Windows window procedure converts its physical
// coordinates once and maps the result onto the HT* codes.
enum class WindowChromeHit {
    Client,   // HTCLIENT
    Caption,  // HTCAPTION
    Top,
    Bottom,
    Left,
    Right,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

struct WindowChromeHitTest {
    // Window rectangle in screen coordinates, sized from its top-left corner:
    // the exclusive right/bottom edges are x()+width()/y()+height(), the Win32
    // RECT convention (LayoutQt.h).
    QRect windowRect;
    // Cursor in the same space as windowRect (physical pixels).
    QPoint cursor;
    // SM_CXFRAME + SM_CXPADDEDBORDER and SM_CYFRAME + SM_CXPADDEDBORDER at the
    // window DPI (the legacy WindowFrameThicknessX/Y, MessageLoop.cpp:68-70).
    int frameThicknessX = 0;
    int frameThicknessY = 0;
    bool maximized = false;
    // Client cursor inside the visible toolbar band [0, VisibleToolbarHeight()).
    bool cursorInBand = false;
    // That band position is a toolbar control (IsToolbarControlRect), i.e. it
    // must stay clickable instead of becoming a caption drag area.
    bool cursorOverToolbarControl = false;
};

// Reference order: a maximized window has no resize zones at all
// (MessageLoop.cpp:90), then the eight frame edges, then the band's
// caption/client split, then plain client.
inline WindowChromeHit ResolveWindowChromeHit(const WindowChromeHitTest& test) {
    if (!test.maximized) {
        const int right = test.windowRect.x() + test.windowRect.width();
        const int bottom = test.windowRect.y() + test.windowRect.height();
        const bool onLeft = test.cursor.x() < test.windowRect.x() + test.frameThicknessX;
        const bool onRight = test.cursor.x() >= right - test.frameThicknessX;
        const bool onTop = test.cursor.y() < test.windowRect.y() + test.frameThicknessY;
        const bool onBottom = test.cursor.y() >= bottom - test.frameThicknessY;

        if (onTop && onLeft) {
            return WindowChromeHit::TopLeft;
        }
        if (onTop && onRight) {
            return WindowChromeHit::TopRight;
        }
        if (onBottom && onLeft) {
            return WindowChromeHit::BottomLeft;
        }
        if (onBottom && onRight) {
            return WindowChromeHit::BottomRight;
        }
        if (onTop) {
            return WindowChromeHit::Top;
        }
        if (onBottom) {
            return WindowChromeHit::Bottom;
        }
        if (onLeft) {
            return WindowChromeHit::Left;
        }
        if (onRight) {
            return WindowChromeHit::Right;
        }
    }

    if (test.cursorInBand) {
        return test.cursorOverToolbarControl ? WindowChromeHit::Client : WindowChromeHit::Caption;
    }
    return WindowChromeHit::Client;
}

}  // namespace cnote::ui
