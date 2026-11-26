#include "ui/WindowBackdropQt.h"

#include <QGuiApplication>
#include <QWidget>

#if defined(Q_OS_WIN)
#include <QOperatingSystemVersion>

#include <windows.h>
#include <dwmapi.h>
#endif

namespace cnote::ui {

namespace {

#if defined(Q_OS_WIN)
// Windows 11 SDK values the toolchain's dwmapi.h predates.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;     // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kDwmSystemBackdropType = 38;       // DWMWA_SYSTEMBACKDROP_TYPE
constexpr int kDwmBackdropAuto = 0;                // DWMSBT_AUTO
constexpr int kDwmBackdropMainWindow = 2;          // DWMSBT_MAINWINDOW: Mica
constexpr int kDwmBackdropTransientWindow = 3;     // DWMSBT_TRANSIENTWINDOW: Acrylic

// The DWM needs a real HWND: the offscreen platform the tests and headless
// runs use on Windows hands out window ids that are not.
bool NativeWindowsPlatform() {
    return QGuiApplication::platformName() == QStringLiteral("windows");
}

bool SystemBackdropsAvailable() {
    return QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11_22H2;
}
#endif

}  // namespace

bool WindowBackdropSupported(settings::CanvasBackdrop backdrop) {
    switch (backdrop) {
    case settings::CanvasBackdrop::Solid:
        return true;
    case settings::CanvasBackdrop::Transparent:
#if defined(Q_OS_WIN)
        return NativeWindowsPlatform();
#else
        return false;
#endif
    case settings::CanvasBackdrop::Mica:
    case settings::CanvasBackdrop::Acrylic:
#if defined(Q_OS_WIN)
        return NativeWindowsPlatform() && SystemBackdropsAvailable();
#else
        return false;
#endif
    }
    return false;
}

settings::CanvasBackdrop ApplyWindowBackdrop(QWidget& window, settings::CanvasBackdrop backdrop) {
    using settings::CanvasBackdrop;
    const CanvasBackdrop effective = WindowBackdropSupported(backdrop) ? backdrop : CanvasBackdrop::Solid;
#if defined(Q_OS_WIN)
    if (!NativeWindowsPlatform()) {
        return effective;
    }
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());

    // Transparent: blur-behind over the whole window makes DWM composite the
    // client area with its per-pixel alpha (Windows 8 and later no longer
    // blur it).  Qt makes the same call for its translucent GL windows.
    const bool transparent = effective == CanvasBackdrop::Transparent;
    DWM_BLURBEHIND blurBehind {};
    blurBehind.dwFlags = DWM_BB_ENABLE;
    blurBehind.fEnable = transparent ? TRUE : FALSE;
    if (transparent) {
        blurBehind.dwFlags |= DWM_BB_BLURREGION;
        blurBehind.hRgnBlur = CreateRectRgn(0, 0, -1, -1);
    }
    DwmEnableBlurBehindWindow(hwnd, &blurBehind);
    if (blurBehind.hRgnBlur != nullptr) {
        DeleteObject(blurBehind.hRgnBlur);
    }

    // Mica and Acrylic: the system material fills the frame, and the frame
    // extended over the whole client area shows under every transparent pixel.
    const bool material = effective == CanvasBackdrop::Mica || effective == CanvasBackdrop::Acrylic;
    const int extent = material ? -1 : 0;
    const MARGINS margins { extent, extent, extent, extent };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    if (SystemBackdropsAvailable()) {
        if (material) {
            // The material takes the window's light/dark mode; cnote's chrome is dark.
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
        }
        const int type = effective == CanvasBackdrop::Mica
            ? kDwmBackdropMainWindow
            : (effective == CanvasBackdrop::Acrylic ? kDwmBackdropTransientWindow : kDwmBackdropAuto);
        DwmSetWindowAttribute(hwnd, kDwmSystemBackdropType, &type, sizeof(type));
    }
#else
    static_cast<void>(window);
#endif
    return effective;
}

}  // namespace cnote::ui
