#pragma once

#include <algorithm>

#include <QRect>
#include <QSize>

namespace cnote::ui {

// Window behavior for the Qt shell (migration plan step 6.3).
//
// The reference application starts at a 1400x960 logical client size
// (README/plan step 6.3) and stays resizable.  Native OS decorations are kept,
// so the Qt shell must *not* reproduce the Win32 frame inset or nonclient hit
// testing: use QWidget::resize() with the result below and let the platform own
// the frame.  The reference in-client toolbar band is still custom inside the
// client area, and the caption-button reserve
// (cnote::appui::qt::CaptionButtonReserve) stays in the layout even though the
// native decorations supply the actual buttons.
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

}  // namespace cnote::ui
