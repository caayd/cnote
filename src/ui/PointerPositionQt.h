#pragma once

#include <optional>

#include <QPoint>

namespace cnote::ui {

// Global pointer position for UI that anchors to the cursor (the quick-size
// and text-format popups, the palette wheel, the zoom anchor, sidebar key
// ownership).  Normally the OS cursor.  The headless capture driver
// (shell/CaptureDriver.h) drives hidden windows with synthetic input and sets
// its synthetic pointer here, so those windows follow the scripted pointer
// instead of reading - or moving - the user's real cursor.
[[nodiscard]] QPoint GlobalCursorPos();
void SetSyntheticCursorPos(std::optional<QPoint> globalPosition);

}  // namespace cnote::ui
