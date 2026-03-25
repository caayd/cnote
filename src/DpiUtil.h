#pragma once

#include <windows.h>

namespace cnote {

[[nodiscard]] UINT SystemDpi();
[[nodiscard]] UINT WindowDpi(HWND window);
[[nodiscard]] int ScaleForDpi(int value, UINT dpi);
[[nodiscard]] float ScaleForDpi(float value, UINT dpi);
[[nodiscard]] int SystemMetricForDpi(int index, UINT dpi);
BOOL AdjustWindowRectForDpi(RECT* rect, DWORD style, BOOL hasMenu, DWORD exStyle, UINT dpi);
[[nodiscard]] int FontHeightForDpi(int height, UINT dpi);

}  // namespace cnote
