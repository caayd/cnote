#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

namespace cnote::ui {

inline constexpr COLORREF kWindowBackground = RGB(18, 22, 29);
inline constexpr COLORREF kPanelFill = RGB(18, 24, 32);
inline constexpr COLORREF kPanelBorder = RGB(46, 58, 74);
inline constexpr COLORREF kButtonFill = RGB(28, 34, 43);
inline constexpr COLORREF kButtonFillActive = RGB(48, 68, 94);
inline constexpr COLORREF kButtonFillHover = RGB(38, 48, 61);
inline constexpr COLORREF kInputFill = RGB(23, 30, 39);
inline constexpr COLORREF kTextPrimary = RGB(235, 240, 246);
inline constexpr COLORREF kTextMuted = RGB(141, 152, 167);
inline constexpr COLORREF kSelectionBorder = RGB(244, 247, 252);
inline constexpr COLORREF kAccent = RGB(92, 137, 187);
inline constexpr COLORREF kScrollbarTrackFill = RGB(20, 26, 34);
inline constexpr COLORREF kScrollbarThumbFill = RGB(70, 88, 111);
inline constexpr COLORREF kScrollbarThumbActiveFill = RGB(92, 118, 150);
inline constexpr int kScrollbarGap = 8;
inline constexpr int kScrollbarWidth = 12;
inline constexpr int kScrollbarMinThumbHeight = 44;

struct HslColor {
    float hue = 0.0f;
    float saturation = 0.0f;
    float lightness = 0.0f;
};

RECT MakeRect(int left, int top, int right, int bottom);
RECT MakeRectFromFloatBounds(float left, float top, float right, float bottom);
bool HasRect(const RECT& rect);
std::wstring TrimWide(std::wstring_view value);
std::string TrimAscii(std::string_view value);
// Temporary native-wide boundary for the Win32 frontend. Model UTF-16 maps one
// 16-bit code unit per wchar; these disappear with the Win32 frontend.
std::wstring WideFromUtf16(std::u16string_view value);
std::u16string Utf16FromWide(std::wstring_view value);
std::wstring FormatHexColor(std::uint32_t color);
std::optional<std::uint32_t> ParseHexColor(std::wstring value);
HslColor RgbToHsl(std::uint32_t color);
std::uint32_t HslToColor(float hueDegrees, float saturation, float lightness);
std::wstring FormatHueLabel(int hue);
std::wstring FormatPercentLabel(const wchar_t* prefix, int value);
std::wstring FormatZoomPercentValue(float zoom);
void SetControlText(HWND control, const std::wstring& text);
LRESULT CALLBACK EditReturnToParentSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR);
void FillRoundedRect(HDC hdc, RECT rect, COLORREF color);
void StrokeRoundedRect(HDC hdc, RECT rect, COLORREF color, int width);
RECT SliderTrackRect(const RECT& sliderRect, UINT dpi);
int SliderThumbCenterX(const RECT& trackRect, int value, int minimum, int maximum);
void FillHuePreviewTrack(HDC hdc, const RECT& trackRect, float saturation, float lightness);
void FillSaturationPreviewTrack(HDC hdc, const RECT& trackRect, float hue, float lightness);
void FillLightnessPreviewTrack(HDC hdc, const RECT& trackRect, float hue, float saturation);
RECT UnionRectangles(RECT a, RECT b);
RECT InflateRectCopy(RECT rect, int amount);
void DeferMoveControl(HDWP& deferred, HWND control, const RECT& rect, UINT flags = SWP_NOZORDER | SWP_NOACTIVATE);

}  // namespace cnote::ui
