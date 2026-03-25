#include "DpiUtil.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

namespace cnote {

UINT SystemDpi() {
    using GetDpiForSystemFn = UINT(WINAPI*)();
    static const auto getDpiForSystem = reinterpret_cast<GetDpiForSystemFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    if (getDpiForSystem != nullptr) {
        return getDpiForSystem();
    }

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        return USER_DEFAULT_SCREEN_DPI;
    }

    const int dpi = GetDeviceCaps(screenDc, LOGPIXELSX);
    ReleaseDC(nullptr, screenDc);
    return dpi > 0 ? static_cast<UINT>(dpi) : USER_DEFAULT_SCREEN_DPI;
}

UINT WindowDpi(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (getDpiForWindow != nullptr && window != nullptr) {
        return getDpiForWindow(window);
    }

    return SystemDpi();
}

int ScaleForDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

float ScaleForDpi(float value, UINT dpi) {
    return value * static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
}

int SystemMetricForDpi(int index, UINT dpi) {
    using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
    static const auto getSystemMetricsForDpi = reinterpret_cast<GetSystemMetricsForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    if (getSystemMetricsForDpi != nullptr) {
        return getSystemMetricsForDpi(index, dpi);
    }

    return MulDiv(GetSystemMetrics(index), static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

BOOL AdjustWindowRectForDpi(RECT* rect, DWORD style, BOOL hasMenu, DWORD exStyle, UINT dpi) {
    using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto adjustWindowRectExForDpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
    if (adjustWindowRectExForDpi != nullptr) {
        return adjustWindowRectExForDpi(rect, style, hasMenu, exStyle, dpi);
    }

    return AdjustWindowRectEx(rect, style, hasMenu, exStyle);
}

int FontHeightForDpi(int height, UINT dpi) {
    const int scaledHeight = ScaleForDpi(height, dpi);
    return scaledHeight + (dpi > USER_DEFAULT_SCREEN_DPI ? 1 : 0);
}

}  // namespace cnote

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
