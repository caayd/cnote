#pragma once

#include "settings/SettingsStoreQt.h"

class QWidget;

namespace cnote::ui {

// Whether this system can show `backdrop` behind a window: Solid everywhere,
// Transparent on Windows (DWM per-pixel alpha), Mica and Acrylic on Windows 11
// 22H2 and later (DWMWA_SYSTEMBACKDROP_TYPE).
[[nodiscard]] bool WindowBackdropSupported(settings::CanvasBackdrop backdrop);

// Puts `backdrop` behind the top-level `window` and returns the one in effect:
// Solid when the system does not support `backdrop`.  Whatever the window
// leaves transparent shows the backdrop, so the window must have had
// Qt::WA_TranslucentBackground before its native window was created.
settings::CanvasBackdrop ApplyWindowBackdrop(QWidget& window, settings::CanvasBackdrop backdrop);

}  // namespace cnote::ui
